/*
 * QuietDatabase.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/actorcompiler.h"
#include "flow/ActorCollection.h"
#include "fdbrpc/simulator.h"
#include "flow/Trace.h"
#include "fdbclient/NativeAPI.h"
#include "fdbclient/DatabaseContext.h"
#include "TesterInterface.h"
#include "WorkerInterface.h"
#include "ServerDBInfo.h"
#include "Status.h"
#include "fdbclient/ManagementAPI.h"

ACTOR Future<vector<std::pair<WorkerInterface, ProcessClass>>> getWorkers( Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	loop {
		choose {
			when( vector<std::pair<WorkerInterface, ProcessClass>> w = wait( brokenPromiseToNever( dbInfo->get().clusterInterface.getWorkers.getReply( GetWorkersRequest() ) ) ) ) {
				return w;
			}
			when( Void _ = wait( dbInfo->onChange() ) ) {}
		}
	}
}

//Gets the WorkerInterface representing the Master server.
ACTOR Future<WorkerInterface> getMasterWorker( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	TraceEvent("GetMasterWorker").detail("Database", printable(cx->dbName)).detail("Stage", "GettingWorkers");

	loop {
		state vector<std::pair<WorkerInterface, ProcessClass>> workers = wait( getWorkers( dbInfo ) );

		for( int i = 0; i < workers.size(); i++ ) {
			if( workers[i].first.address() == dbInfo->get().master.address() ) {
				TraceEvent("GetMasterWorker").detail("Database", printable(cx->dbName)).detail("Stage", "GotWorkers").detail("masterId", dbInfo->get().master.id()).detail("workerId", workers[i].first.id());
				return workers[i].first;
			}
		}

		TraceEvent(SevWarn, "GetMasterWorkerError")
			.detail("Database", printable(cx->dbName)).detail("Error", "MasterWorkerNotFound")
			.detail("Master", dbInfo->get().master.id()).detail("MasterAddress", dbInfo->get().master.address())
			.detail("WorkerCount", workers.size());

		Void _ = wait(delay(1.0));
	}
}

//Gets the number of bytes in flight from the master
ACTOR Future<int64_t> getDataInFlight( Database cx, WorkerInterface masterWorker ) {
	try {
		TraceEvent("DataInFlight").detail("Database", printable(cx->dbName)).detail("Stage", "ContactingMaster");
		Standalone<StringRef> md = wait( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( cx->dbName.toString() + "/TotalDataInFlight" ) ) ), 1.0 ) );
		int64_t dataInFlight;
		sscanf(extractAttribute(md.toString(), "TotalBytes").c_str(), "%lld", &dataInFlight);
		return dataInFlight;
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", masterWorker.id()).detail("Reason", "Failed to extract DataInFlight");
		throw;
	}

}

//Gets the number of bytes in flight from the master
//Convenience method that first finds the master worker from a zookeeper interface
ACTOR Future<int64_t> getDataInFlight( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	WorkerInterface masterWorker = wait(getMasterWorker(cx, dbInfo));
	int64_t dataInFlight = wait(getDataInFlight(cx, masterWorker));
	return dataInFlight;
}

//Computes the queue size for storage servers and tlogs using the BytesInput and BytesDurable attributes
//For now, we must ignore invalid keys on storage servers because of a bug that can cause them to be orphaned
int64_t getQueueSize( Standalone<StringRef> md ) {
	int64_t bytesInput, bytesDurable;

	sscanf(extractAttribute(md.toString(), "BytesInput").c_str(), "%lld", &bytesInput);
	sscanf(extractAttribute(md.toString(), "BytesDurable").c_str(), "%lld", &bytesDurable);

	return bytesInput - bytesDurable;
}

// This is not robust in the face of a TLog failure
ACTOR Future<int64_t> getMaxTLogQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, WorkerInterface masterWorker ) {
	TraceEvent("MaxTLogQueueSize").detail("Database", printable(cx->dbName))
		.detail("Stage", "ContactingMaster");

	state std::vector<Future<Standalone<StringRef>>> messages;
	state std::vector<TLogInterface> tlogs = dbInfo->get().logSystemConfig.allPresentLogs();
	for(int i = 0; i < tlogs.size(); i++) {
		messages.push_back( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( "TLogQueueSize/" + tlogs[i].id().toString() ) ) ), 1.0 ) );
	}
	Void _ = wait( waitForAll( messages ) );

	TraceEvent("MaxTLogQueueSize").detail("Database", printable(cx->dbName))
		.detail("Stage", "ComputingMax").detail("MessageCount", messages.size());

	state int64_t maxQueueSize = 0;
	state int i = 0;
	for(; i < messages.size(); i++) {
		try {
			maxQueueSize = std::max( maxQueueSize, getQueueSize( messages[i].get() ) );
		} catch( Error &e ) {
			TraceEvent("QuietDatabaseFailure", masterWorker.id()).detail("Reason", "Failed to extract MaxTLogQueue").detail("Tlog", tlogs[i].id());
			throw;
		}
	}

	return maxQueueSize;
}

ACTOR Future<int64_t> getMaxTLogQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	WorkerInterface masterWorker = wait(getMasterWorker(cx, dbInfo));
	int64_t maxQueueSize = wait(getMaxTLogQueueSize(cx, dbInfo, masterWorker));
	return maxQueueSize;
}

ACTOR Future<vector<StorageServerInterface>> getStorageServers( Database cx, bool use_system_priority = false) {
	state Transaction tr( cx );
	if (use_system_priority)
		tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
	loop {
		try {
			Standalone<RangeResultRef> serverList = wait( tr.getRange( serverListKeys, CLIENT_KNOBS->TOO_MANY ) );
			ASSERT( !serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY );

			vector<StorageServerInterface> servers;
			for( int i = 0; i < serverList.size(); i++ )
				servers.push_back( decodeServerListValue( serverList[i].value ) );
			return servers;
		}
		catch(Error &e) {
			Void _ = wait( tr.onError(e) );
		}
	}
}

//Gets the maximum size of all the storage server queues
ACTOR Future<int64_t> getMaxStorageServerQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, WorkerInterface masterWorker ) {
	TraceEvent("MaxStorageServerQueueSize").detail("Database", printable(cx->dbName)).detail("Stage", "ContactingMaster");

	state vector<StorageServerInterface> servers = wait( getStorageServers( cx ) );
	state std::vector<Future<Standalone<StringRef>>> messages;
	for(int i = 0; i < servers.size(); i++) {
		messages.push_back( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( "StorageServerQueueSize/" + servers[i].id().toString() ) ) ), 1.0 ) );
	}

	Void _ = wait( waitForAll(messages) );

	TraceEvent("MaxStorageServerQueueSize").detail("Database", printable(cx->dbName)).detail("Stage", "ComputingMax").detail("MessageCount", messages.size());

	state int64_t maxQueueSize = 0;
	state int i = 0;
	for(; i < messages.size(); i++) {
		try {
			maxQueueSize = std::max( maxQueueSize, getQueueSize( messages[i].get() ) );
		} catch( Error &e ) {
			TraceEvent("QuietDatabaseFailure", masterWorker.id()).detail("Reason", "Failed to extract MaxStorageServerQueue").detail("SS", servers[i].id());
			throw;
		}
	}

	return maxQueueSize;
}

//Gets the maximum size of all the storage server queues
//Convenience method that first gets the master worker and system map from a zookeeper interface
ACTOR Future<int64_t> getMaxStorageServerQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	WorkerInterface masterWorker = wait(getMasterWorker(cx, dbInfo));
	int64_t maxQueueSize = wait(getMaxStorageServerQueueSize(cx, dbInfo, masterWorker));
	return maxQueueSize;
}

//Gets the size of the data distribution queue.  If reportInFlight is true, then data in flight is considered part of the queue
ACTOR Future<int64_t> getDataDistributionQueueSize( Database cx, WorkerInterface masterWorker, bool reportInFlight) {
	try {
		TraceEvent("DataDistributionQueueSize").detail("Database", printable(cx->dbName)).detail("Stage", "ContactingMaster");

		Standalone<StringRef> movingDataMessage = wait( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( cx->dbName.toString() + "/MovingData") ) ), 1.0 ) );

		TraceEvent("DataDistributionQueueSize").detail("Database", printable(cx->dbName)).detail("Stage", "GotString").detail("Result", printable(movingDataMessage)).detail("TrackLatest", printable( StringRef( cx->dbName.toString() + "/MovingData") ) );

		int64_t inQueue;
		sscanf(extractAttribute(movingDataMessage.toString(), "InQueue").c_str(), "%lld", &inQueue);

		if(reportInFlight) {
			int64_t inFlight;
			sscanf(extractAttribute(movingDataMessage.toString(), "InFlight").c_str(), "%lld", &inFlight);
			inQueue += inFlight;
		}

		return inQueue;
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", masterWorker.id()).detail("Reason", "Failed to extract DataDistributionQueueSize");
		throw;
	}
}

//Gets the size of the data distribution queue.  If reportInFlight is true, then data in flight is considered part of the queue
//Convenience method that first finds the master worker from a zookeeper interface
ACTOR Future<int64_t> getDataDistributionQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, bool reportInFlight ) {
	WorkerInterface masterWorker = wait(getMasterWorker(cx, dbInfo));
	int64_t inQueue = wait(getDataDistributionQueueSize( cx, masterWorker, reportInFlight));
	return inQueue;
}

//Checks that data distribution is active
ACTOR Future<bool> getDataDistributionActive( Database cx, WorkerInterface masterWorker ) {
	try {
		TraceEvent("DataDistributionActive").detail("Database", printable(cx->dbName)).detail("Stage", "ContactingMaster");

		Standalone<StringRef> activeMessage = wait( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( cx->dbName.toString() + "/DDTrackerStarting") ) ), 1.0 ) );

		return extractAttribute(activeMessage.toString(), "State") == "Active";
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", masterWorker.id()).detail("Reason", "Failed to extract DataDistributionActive");
		throw;
	}
}

//Checks to see if any storage servers are being recruited
ACTOR Future<bool> getStorageServersRecruiting( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, WorkerInterface masterWorker ) {
	try {
		TraceEvent("StorageServersRecruiting").detail("Database", printable(cx->dbName)).detail("Stage", "ContactingMaster");

		Standalone<StringRef> recruitingMessage = wait( timeoutError(masterWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( cx->dbName.toString() + "/StorageServerRecruitment_" + dbInfo->get().master.id().toString()) ) ), 1.0 ) );

		return extractAttribute(recruitingMessage.toString(), "State") == "Recruiting";
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", masterWorker.id()).detail("Reason", "Failed to extract StorageServersRecruiting").detail("MasterID", dbInfo->get().master.id());
		throw;
	}
}

ACTOR Future<Void> waitForQuietDatabase( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, std::string phase, int64_t dataInFlightGate = 2e6,
	int64_t maxTLogQueueGate = 5e6, int64_t maxStorageServerQueueGate = 5e6, int64_t maxDataDistributionQueueSize = 0 ) {

	TraceEvent(("QuietDatabase" + phase + "Begin").c_str());

	//In a simulated environment, wait 5 seconds so that workers can move to their optimal locations
	if(g_network->isSimulated())
		Void _ = wait(delay(5.0));

	//Require 2 consecutive successful quiet database checks spaced 1 second apart
	state int numSuccesses = 0;

	loop {
		try {
			TraceEvent("QuietDatabaseWaitingOnMaster");
			WorkerInterface masterWorker = wait(getMasterWorker( cx, dbInfo ));
			TraceEvent("QuietDatabaseGotMaster");

			state Future<int64_t> dataInFlight = getDataInFlight( cx, masterWorker);
			state Future<int64_t> tLogQueueSize = getMaxTLogQueueSize( cx, dbInfo, masterWorker );
			state Future<int64_t> dataDistributionQueueSize = getDataDistributionQueueSize( cx, masterWorker, dataInFlightGate == 0);
			state Future<int64_t> storageQueueSize = getMaxStorageServerQueueSize( cx, dbInfo, masterWorker );
			state Future<bool> dataDistributionActive = getDataDistributionActive( cx, masterWorker );
			state Future<bool> storageServersRecruiting = getStorageServersRecruiting ( cx, dbInfo, masterWorker );

			Void _ = wait( success( dataInFlight ) && success( tLogQueueSize ) && success( dataDistributionQueueSize )
							&& success( storageQueueSize ) && success( dataDistributionActive ) && success( storageServersRecruiting ) );
			TraceEvent(("QuietDatabase" + phase).c_str())
				.detail("dataInFlight", dataInFlight.get()).detail("maxTLogQueueSize", tLogQueueSize.get()).detail("dataDistributionQueueSize", dataDistributionQueueSize.get())
				.detail("maxStorageQueueSize", storageQueueSize.get()).detail("dataDistributionActive", dataDistributionActive.get())
				.detail("storageServersRecruiting", storageServersRecruiting.get());

			if( dataInFlight.get() > dataInFlightGate || tLogQueueSize.get() > maxTLogQueueGate
				|| dataDistributionQueueSize.get() > maxDataDistributionQueueSize || storageQueueSize.get() > maxStorageServerQueueGate
				|| dataDistributionActive.get() == false || storageServersRecruiting.get() == true) {

				Void _ = wait( delay( 1.0 ) );
				numSuccesses = 0;
			} else {
				if(++numSuccesses == 2) {
					TraceEvent(("QuietDatabase" + phase + "Done").c_str());
					break;
				}
				else
					Void _ = wait(delay( 2.0 ) );
			}
		} catch (Error& e) {
			if( e.code() != error_code_actor_cancelled && e.code() != error_code_attribute_not_found && e.code() != error_code_timed_out)
				TraceEvent(("QuietDatabase" + phase + "Error").c_str()).error(e);

			//Client invalid operation occurs if we don't get back a message from one of the servers, often corrected by retrying
			if(e.code() != error_code_attribute_not_found && e.code() != error_code_timed_out)
				throw;

			TraceEvent(("QuietDatabase" + phase + "Retry").c_str()).error(e);
			Void _ = wait(delay(1.0));
			numSuccesses = 0;
		}
	}

	return Void();
}

//Waits for f to complete. If simulated, disables connection failures after waiting a specified amount of time
ACTOR Future<Void> disableConnectionFailuresAfter( Future<Void> f, double disableTime, std::string context ) {
	if(!g_network->isSimulated()) {
		Void _ = wait(f);
		return Void();
	}

	choose {
		when(Void _ = wait(f)) {
			return Void();
		}
		when(Void _ = wait(delay(disableTime))) {
			g_simulator.speedUpSimulation = true;
			g_simulator.enableConnectionFailures = false;
			TraceEvent(SevWarnAlways, ("DisableConnectionFailures_" + context).c_str());
		}
	}

	Void _ = wait(f);
	return Void();
}


Future<Void> quietDatabase( Database const& cx, Reference<AsyncVar<ServerDBInfo>> const& dbInfo, std::string phase, int64_t dataInFlightGate,
	int64_t maxTLogQueueGate, int64_t maxStorageServerQueueGate, int64_t maxDataDistributionQueueSize ) {

	Future<Void> quiet = waitForQuietDatabase(cx, dbInfo, phase, dataInFlightGate, maxTLogQueueGate, maxStorageServerQueueGate, maxDataDistributionQueueSize);
	return disableConnectionFailuresAfter(quiet, 300.0, "QuietDatabase" + phase);
}