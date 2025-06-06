/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/***********************************************************************
 *   $Id: ddlprocessor.cpp 6 2006-06-23 17:58:51Z rcraighead $
 *
 *
 ***********************************************************************/
#include <map>
#include <boost/scoped_ptr.hpp>
using namespace std;

#include "ddlpkg.h"
#include "ddlprocessor.h"
#include "createtableprocessor.h"
#include "altertableprocessor.h"
#include "droptableprocessor.h"
#include "calpontsystemcatalog.h"
#include "sqlparser.h"
#include "configcpp.h"
#include "markpartitionprocessor.h"
#include "restorepartitionprocessor.h"
#include "droppartitionprocessor.h"
//#define 	SERIALIZE_DDL_DML_CPIMPORT 	1

#include "cacheutils.h"
#include "vss.h"
#include "dbrm.h"
#include "idberrorinfo.h"
#include "errorids.h"
#include "we_messages.h"
using namespace BRM;

using namespace config;
using namespace messageqcpp;
using namespace ddlpackageprocessor;
using namespace ddlpackage;
using namespace execplan;
using namespace logging;
using namespace WriteEngine;

#include "querytele.h"
using namespace querytele;

#include "oamcache.h"

namespace
{
typedef messageqcpp::ByteStream::quadbyte quadbyte;

const quadbyte UNRECOGNIZED_PACKAGE_TYPE = 100;

const std::string DDLProcName = "DDLProc";

void cleanPMSysCache()
{
  vector<BRM::OID_t> oidList = getAllSysCatOIDs();
  cacheutils::flushOIDsFromCache(oidList);
}

class PackageHandler
{
 public:
  PackageHandler(QueryTeleClient qtc, DBRM* dbrm, messageqcpp::IOSocket& ios, bool concurrentSupport,
                 uint32_t* debugLevel)
   : fIos(ios), fDbrm(dbrm), fQtc(qtc), fConcurrentSupport(concurrentSupport), fDebugLevel(debugLevel)
  {
  }

  void operator()()
  {
    try
    {
      fByteStream = fIos.read();
      if (fByteStream.empty())
      {
        logError(DDLPackageProcessor::NOT_ACCEPTING_PACKAGES, "Empty package", true);
        return;
      }
      fByteStream >> fSessionID;
      fByteStream >> fPackageType;

      uint32_t stateFlags;
      if (fDbrm->getSystemState(stateFlags) >
          0)  // > 0 implies successful retrieval. It doesn't imply anything about the contents
      {
        messageqcpp::ByteStream results;
        const char* responseMsg = nullptr;
        messageqcpp::ByteStream::byte status;
        bool bReject = false;

        // Check to see if we're in write suspended or shutdown mode
        // If so, we can't process the request.
        if (stateFlags & SessionManagerServer::SS_SUSPENDED ||
            stateFlags & SessionManagerServer::SS_SUSPEND_PENDING ||
            stateFlags & SessionManagerServer::SS_SHUTDOWN_PENDING)
        {
          if (stateFlags & SessionManagerServer::SS_SUSPENDED ||
              stateFlags & SessionManagerServer::SS_SUSPEND_PENDING)
          {
            responseMsg = "Writing to the database is disabled.";
          }
          else
          {
            responseMsg = "The database is being shut down.";
          }

          status = DDLPackageProcessor::NOT_ACCEPTING_PACKAGES;
          bReject = true;
        }

        if (bReject)
        {
          results << status;
          //@bug 266
          MessageLog logger(LoggingID(27));
          logging::Message::Args args;
          logging::Message message(2);
          args.add(responseMsg);
          message.format(args);
          results << message.msg();
          fIos.write(results);
          std::cout << responseMsg << endl;
          std::cout << "Command rejected. Status " << (int)status << message.msg() << endl;
          return;
        }
      }

      // check whether the system is ready to process statement.
      if (fDbrm->getSystemReady() < 1)
      {
        logError(DDLPackageProcessor::NOT_ACCEPTING_PACKAGES, "System is not ready yet. Please try again.",
                 true);
        return;
      }

      int rc = 0;

      if (!fConcurrentSupport)
      {
        // Check if any other active transaction
        bool bIsDbrmUp = true;
        bool anyOtherActiveTransaction = true;
        execplan::SessionManager sessionManager;
        BRM::SIDTIDEntry blockingsid;
        int i = 0;
        int waitPeriod = 10;
        //@Bug 2487 Check transaction map every 1/10 second

        int sleepTime = 100;  // sleep 100 milliseconds between checks
        int numTries = 10;    // try 10 times per second

        string waitPeriodStr = config::Config::makeConfig()->getConfig("SystemConfig", "WaitPeriod");

        if (waitPeriodStr.length() != 0)
          waitPeriod = static_cast<int>(config::Config::fromText(waitPeriodStr));

        numTries = waitPeriod * 10;
        struct timespec rm_ts;

        rm_ts.tv_sec = sleepTime / 1000;
        rm_ts.tv_nsec = sleepTime % 1000 * 1000000;

        // cout << "starting i = " << i << endl;
        // anyOtherActiveTransaction = sessionManager.checkActiveTransaction(fSessionID, bIsDbrmUp);
        while (anyOtherActiveTransaction)
        {
          anyOtherActiveTransaction =
              sessionManager.checkActiveTransaction(fSessionID, bIsDbrmUp, blockingsid);

          if (anyOtherActiveTransaction)
          {
            for (; i < numTries; i++)
            {
              struct timespec abs_ts;

              // cout << "session " << fSessionID << " nanosleep on package type " << (int)packageType <<
              // endl;
              do
              {
                abs_ts.tv_sec = rm_ts.tv_sec;
                abs_ts.tv_nsec = rm_ts.tv_nsec;
              } while (nanosleep(&abs_ts, &rm_ts) < 0);

              anyOtherActiveTransaction =
                  sessionManager.checkActiveTransaction(fSessionID, bIsDbrmUp, blockingsid);

              if (!anyOtherActiveTransaction)
              {
                // cout << "Ready to process type " << (int)packageType << endl;
                fTxnid = sessionManager.getTxnID(fSessionID);

                if (!fTxnid.valid)
                {
                  fTxnid = sessionManager.newTxnID(fSessionID, true, true);

                  if (fTxnid.valid)
                  {
                    // cout << "Ready to process type " << (int)packageType << " with txnid " << txnid.id <<
                    // endl;
                    anyOtherActiveTransaction = false;
                    break;
                  }
                  else
                  {
                    anyOtherActiveTransaction = true;
                  }
                }
                else
                {
                  string errorMsg;
                  rc = commitTransaction(fTxnid.id, errorMsg);

                  if (rc != 0)
                    throw std::runtime_error(errorMsg);

                  // need unlock the table.
                  std::vector<TableLockInfo> tableLocks = fDbrm->getAllTableLocks();
                  bool lockReleased = true;

                  for (unsigned k = 0; k < tableLocks.size(); k++)
                  {
                    if (tableLocks[k].ownerTxnID == fTxnid.id)
                    {
                      lockReleased = fDbrm->releaseTableLock(tableLocks[k].id);

                      if (!lockReleased)
                      {
                        ostringstream os;
                        os << "tablelock id " << tableLocks[k].id << " is not found";
                        throw std::runtime_error(os.str());
                      }
                    }
                  }

                  fDbrm->committed(fTxnid);
                  fTxnid = fDbrm->newTxnID(fSessionID, true, true);

                  if (fTxnid.valid)
                  {
                    // cout << "Ready to process type " << (int)packageType << " with txnid " << txnid.id <<
                    // endl;
                    anyOtherActiveTransaction = false;
                    break;
                  }
                  else
                  {
                    anyOtherActiveTransaction = true;
                  }
                }
              }
            }

            // cout << "ending i = " << i << endl;
          }
          else
          {
            // cout << "Ready to process type " << (int)packageType << endl;
            fTxnid = sessionManager.getTxnID(fSessionID);

            if (!fTxnid.valid)
            {
              fTxnid = sessionManager.newTxnID(fSessionID, true, true);

              if (!fTxnid.valid)
              {
                // cout << "cannot get txnid " << (int)packageType << " for session " << sessionID <<  endl;
                anyOtherActiveTransaction = true;
              }
              else
              {
                anyOtherActiveTransaction = false;
              }
            }
            else
            {
              string errorMsg;
              rc = commitTransaction(fTxnid.id, errorMsg);

              if (rc != 0)
                throw std::runtime_error(errorMsg);

              // need unlock the table.
              std::vector<TableLockInfo> tableLocks = fDbrm->getAllTableLocks();
              bool lockReleased = true;

              for (unsigned k = 0; k < tableLocks.size(); k++)
              {
                if (tableLocks[k].ownerTxnID == fTxnid.id)
                {
                  lockReleased = fDbrm->releaseTableLock(tableLocks[k].id);

                  if (!lockReleased)
                  {
                    ostringstream os;
                    os << "tablelock id " << tableLocks[k].id << " is not found";
                    throw std::runtime_error(os.str());
                  }
                }
              }

              sessionManager.committed(fTxnid);
              fTxnid = sessionManager.newTxnID(fSessionID, true, true);

              if (!fTxnid.valid)
              {
                // cout << "cannot get txnid " << (int)packageType << " for session " << sessionID <<  endl;
                anyOtherActiveTransaction = true;
              }
              else
              {
                anyOtherActiveTransaction = false;
              }
            }
          }

          if ((anyOtherActiveTransaction) && (i >= numTries))
          {
            // cout << " Erroring out on package type " << (int)packageType << endl;
            break;
          }
        }

        if ((anyOtherActiveTransaction) && (i >= numTries))
        {
          messageqcpp::ByteStream::byte status = DDLPackageProcessor::NOT_ACCEPTING_PACKAGES;

          Message::Args args;
          args.add(static_cast<uint64_t>(blockingsid.sessionid));

          //@Bug 3854 Log to debug.log
          LoggingID logid(15, 0, 0);
          logging::Message::Args args1;
          logging::Message msg(1);
          args1.add(IDBErrorInfo::instance()->errorMsg(ERR_ACTIVE_TRANSACTION, args));
          msg.format(args1);
          logging::Logger logger(logid.fSubsysID);
          logger.logMessage(LOG_TYPE_DEBUG, msg, logid);

          logError(status, IDBErrorInfo::instance()->errorMsg(ERR_ACTIVE_TRANSACTION, args));
        }
        else
        {
          processStatement();
        }
      }
      else
      {
        fTxnid = fDbrm->getTxnID(fSessionID);

        if (!fTxnid.valid)
        {
          fTxnid = fDbrm->newTxnID(fSessionID, true, true);

          if (!fTxnid.valid)
          {
            throw std::runtime_error(std::string("Unable to start a transaction. Check critical log."));
          }
        }
        else
        {
          string errorMsg;
          rc = commitTransaction(fTxnid.id, errorMsg);

          if (rc != 0)
            throw std::runtime_error(errorMsg);

          // need unlock the table.
          std::vector<TableLockInfo> tableLocks = fDbrm->getAllTableLocks();
          bool lockReleased = true;

          for (unsigned k = 0; k < tableLocks.size(); k++)
          {
            if (tableLocks[k].ownerTxnID == fTxnid.id)
            {
              lockReleased = fDbrm->releaseTableLock(tableLocks[k].id);

              if (!lockReleased)
              {
                ostringstream os;
                os << "tablelock id " << tableLocks[k].id << " is not found";
                throw std::runtime_error(os.str());
              }
            }
          }

          fDbrm->committed(fTxnid);
          fTxnid = fDbrm->newTxnID(fSessionID, true, true);

          if (!fTxnid.valid)
          {
            throw std::runtime_error(std::string("Unable to start a transaction. Check critical log."));
          }
        }

        processStatement();
      }
    }
    catch (const std::exception& ex)
    {
      logError(DDLPackageProcessor::NOT_ACCEPTING_PACKAGES, ex.what(), true);
      throw;
    }
  }

 private:
  int commitTransaction(uint32_t txnID, std::string& errorMsg)
  {
    auto WEClient =
        std::unique_ptr<WriteEngine::WEClients>(new WriteEngine::WEClients(WriteEngine::WEClients::DDLPROC));
    auto PMCount = WEClient->getPmCount();
    ByteStream bytestream;
    uint64_t uniqueId = fDbrm->getUnique64();
    WEClient->addQueue(uniqueId);
    bytestream << (ByteStream::byte)WE_SVR_COMMIT_VERSION;
    bytestream << uniqueId;
    bytestream << txnID;
    uint32_t msgRecived = 0;
    WEClient->write_to_all(bytestream);
    boost::shared_ptr<messageqcpp::ByteStream> bsIn;
    bsIn.reset(new ByteStream());
    int rc = 0;
    ByteStream::byte tmp8;

    while (true)
    {
      if (msgRecived == PMCount)
        break;

      WEClient->read(uniqueId, bsIn);

      if (bsIn->length() == 0)  // read error
      {
        rc = 1;
        errorMsg = "DDL cannot communicate with WES";
        WEClient->removeQueue(uniqueId);
        break;
      }
      else
      {
        *bsIn >> tmp8;
        rc = tmp8;

        if (rc != 0)
        {
          *bsIn >> errorMsg;
          WEClient->removeQueue(uniqueId);
          break;
        }
        else
          msgRecived++;
      }
    }

    return rc;
  }

  void processStatement()
  {
    DDLPackageProcessor::DDLResult result;
    result.result = DDLPackageProcessor::NO_ERROR;
    // boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr;

    try
    {
      if (!fOamCache)
        fOamCache = oam::OamCache::makeOamCache();
      // cout << "DDLProc received package " << fPackageType << endl;
      switch (fPackageType)
      {
        case ddlpackage::DDL_CREATE_TABLE_STATEMENT:
        {
          CreateTableStatement createTableStmt;
          createTableStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(createTableStmt.fSessionID);
          boost::scoped_ptr<CreateTableProcessor> processor(new CreateTableProcessor(fDbrm));
          processor->fTxnid.id = fTxnid.id;
          processor->fTxnid.valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));
          // cout << "create table using txnid " << fTxnid.id << endl;

          QueryTeleStats qts;
          qts.query_uuid = QueryTeleClient::genUUID();
          qts.msg_type = QueryTeleStats::QT_START;
          qts.start_time = QueryTeleClient::timeNowms();
          qts.session_id = createTableStmt.fSessionID;
          qts.query_type = "CREATE";
          qts.query = createTableStmt.fSql;
          qts.system_name = fOamCache->getSystemName();
          qts.module_name = fOamCache->getModuleName();
          qts.schema_name = createTableStmt.schemaName();
          fQtc.postQueryTele(qts);

          result = processor->processPackage(&createTableStmt);

          systemCatalogPtr->removeCalpontSystemCatalog(createTableStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(createTableStmt.fSessionID | 0x80000000);

          qts.msg_type = QueryTeleStats::QT_SUMMARY;
          qts.end_time = QueryTeleClient::timeNowms();
          fQtc.postQueryTele(qts);
        }
        break;

        case ddlpackage::DDL_ALTER_TABLE_STATEMENT:
        {
          AlterTableStatement alterTableStmt;
          alterTableStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(alterTableStmt.fSessionID);
          boost::scoped_ptr<AlterTableProcessor> processor(new AlterTableProcessor(fDbrm));
          processor->fTxnid.id = fTxnid.id;
          processor->fTxnid.valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));

          QueryTeleStats qts;
          qts.query_uuid = QueryTeleClient::genUUID();
          qts.msg_type = QueryTeleStats::QT_START;
          qts.start_time = QueryTeleClient::timeNowms();
          qts.session_id = alterTableStmt.fSessionID;
          qts.query_type = "ALTER";
          qts.query = alterTableStmt.fSql;
          qts.system_name = fOamCache->getSystemName();
          qts.module_name = fOamCache->getModuleName();
          qts.schema_name = alterTableStmt.schemaName();
          fQtc.postQueryTele(qts);

          processor->fTimeZone = alterTableStmt.getTimeZone();

          result = processor->processPackage(&alterTableStmt);

          systemCatalogPtr->removeCalpontSystemCatalog(alterTableStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(alterTableStmt.fSessionID | 0x80000000);

          qts.msg_type = QueryTeleStats::QT_SUMMARY;
          qts.end_time = QueryTeleClient::timeNowms();
          fQtc.postQueryTele(qts);
        }
        break;

        case ddlpackage::DDL_DROP_TABLE_STATEMENT:
        {
          DropTableStatement dropTableStmt;
          dropTableStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(dropTableStmt.fSessionID);
          boost::scoped_ptr<DropTableProcessor> processor(new DropTableProcessor(fDbrm));

          processor->fTxnid.id = fTxnid.id;
          processor->fTxnid.valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));

          QueryTeleStats qts;
          qts.query_uuid = QueryTeleClient::genUUID();
          qts.msg_type = QueryTeleStats::QT_START;
          qts.start_time = QueryTeleClient::timeNowms();
          qts.session_id = dropTableStmt.fSessionID;
          qts.query_type = "DROP";
          qts.query = dropTableStmt.fSql;
          qts.system_name = fOamCache->getSystemName();
          qts.module_name = fOamCache->getModuleName();
          qts.schema_name = dropTableStmt.schemaName();
          fQtc.postQueryTele(qts);

          // cout << "Drop table using txnid " << fTxnid.id << endl;
          result = processor->processPackage(&dropTableStmt);

          systemCatalogPtr->removeCalpontSystemCatalog(dropTableStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(dropTableStmt.fSessionID | 0x80000000);

          qts.msg_type = QueryTeleStats::QT_SUMMARY;
          qts.end_time = QueryTeleClient::timeNowms();
          fQtc.postQueryTele(qts);
        }
        break;

        case ddlpackage::DDL_TRUNC_TABLE_STATEMENT:
        {
          TruncTableStatement truncTableStmt;
          truncTableStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(truncTableStmt.fSessionID);
          boost::scoped_ptr<TruncTableProcessor> processor(new TruncTableProcessor(fDbrm));

          processor->fTxnid.id = fTxnid.id;
          processor->fTxnid.valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));

          QueryTeleStats qts;
          qts.query_uuid = QueryTeleClient::genUUID();
          qts.msg_type = QueryTeleStats::QT_START;
          qts.start_time = QueryTeleClient::timeNowms();
          qts.session_id = truncTableStmt.fSessionID;
          qts.query_type = "TRUNCATE";
          qts.query = truncTableStmt.fSql;
          qts.system_name = fOamCache->getSystemName();
          qts.module_name = fOamCache->getModuleName();
          qts.schema_name = truncTableStmt.schemaName();
          fQtc.postQueryTele(qts);

          result = processor->processPackage(&truncTableStmt);

          systemCatalogPtr->removeCalpontSystemCatalog(truncTableStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(truncTableStmt.fSessionID | 0x80000000);

          qts.msg_type = QueryTeleStats::QT_SUMMARY;
          qts.end_time = QueryTeleClient::timeNowms();
          fQtc.postQueryTele(qts);
        }
        break;

        case ddlpackage::DDL_MARK_PARTITION_STATEMENT:
        {
          MarkPartitionStatement markPartitionStmt;
          markPartitionStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(markPartitionStmt.fSessionID);
          boost::scoped_ptr<MarkPartitionProcessor> processor(new MarkPartitionProcessor(fDbrm));
          (processor->fTxnid).id = fTxnid.id;
          (processor->fTxnid).valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));

          result = processor->processPackage(&markPartitionStmt);
          systemCatalogPtr->removeCalpontSystemCatalog(markPartitionStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(markPartitionStmt.fSessionID | 0x80000000);
        }
        break;

        case ddlpackage::DDL_RESTORE_PARTITION_STATEMENT:
        {
          RestorePartitionStatement restorePartitionStmt;
          restorePartitionStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(restorePartitionStmt.fSessionID);
          boost::scoped_ptr<RestorePartitionProcessor> processor(new RestorePartitionProcessor(fDbrm));
          (processor->fTxnid).id = fTxnid.id;
          (processor->fTxnid).valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));

          result = processor->processPackage(&restorePartitionStmt);
          systemCatalogPtr->removeCalpontSystemCatalog(restorePartitionStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(restorePartitionStmt.fSessionID | 0x80000000);
        }
        break;

        case ddlpackage::DDL_DROP_PARTITION_STATEMENT:
        {
          DropPartitionStatement dropPartitionStmt;
          dropPartitionStmt.unserialize(fByteStream);
          boost::shared_ptr<CalpontSystemCatalog> systemCatalogPtr =
              CalpontSystemCatalog::makeCalpontSystemCatalog(dropPartitionStmt.fSessionID);
          boost::scoped_ptr<DropPartitionProcessor> processor(new DropPartitionProcessor(fDbrm));
          (processor->fTxnid).id = fTxnid.id;
          (processor->fTxnid).valid = true;
          if (fDebugLevel)
            processor->setDebugLevel(static_cast<DDLPackageProcessor::DebugLevel>(*fDebugLevel));

          result = processor->processPackage(&dropPartitionStmt);
          systemCatalogPtr->removeCalpontSystemCatalog(dropPartitionStmt.fSessionID);
          systemCatalogPtr->removeCalpontSystemCatalog(dropPartitionStmt.fSessionID | 0x80000000);
        }
        break;

        case ddlpackage::DDL_DEBUG_STATEMENT:
        {
          DebugDDLStatement stmt;
          stmt.unserialize(fByteStream);
          if (fDebugLevel)
            *fDebugLevel = stmt.fDebugLevel;
        }
        break;

        default: throw UNRECOGNIZED_PACKAGE_TYPE; break;
      }

      //@Bug 3427. No need to log user rror, just return the message to user.
      // Log errors
      if ((result.result != DDLPackageProcessor::NO_ERROR) &&
          (result.result != DDLPackageProcessor::USER_ERROR))
      {
        logging::LoggingID lid(23);
        logging::MessageLog ml(lid);

        ml.logErrorMessage(result.message);
      }

      string hdfstest = config::Config::makeConfig()->getConfig("Installation", "DBRootStorageType");

      if (hdfstest == "hdfs" || hdfstest == "HDFS")
        cleanPMSysCache();

      messageqcpp::ByteStream results;
      messageqcpp::ByteStream::byte status = result.result;
      results << status;
      results << result.message.msg();

      fIos.write(results);

      fIos.close();
    }
    catch (quadbyte& /*foo*/)
    {
      logError(DDLPackageProcessor::NOT_ACCEPTING_PACKAGES, "Unrecognized package type", true);
    }
    catch (logging::IDBExcept& idbEx)
    {
      cleanPMSysCache();
      logError(DDLPackageProcessor::CREATE_ERROR, idbEx.what(), true);
    }
    catch (...)
    {
      logError(DDLPackageProcessor::NOT_ACCEPTING_PACKAGES, "Unknown error", true);
    }
  }

  void logError(messageqcpp::ByteStream::byte status, const std::string& msg, bool closeSocket = false)
  {
    messageqcpp::ByteStream res;
    res << status;
    res << msg;
    fIos.write(res);
    cerr << "DDLProc error: " << msg << endl;
    if (closeSocket)
      fIos.close();
  }

 private:
  messageqcpp::IOSocket fIos;
  messageqcpp::ByteStream fByteStream;
  messageqcpp::ByteStream::quadbyte fPackageType;
  uint32_t fSessionID;
  BRM::TxnID fTxnid;
  BRM::DBRM* fDbrm;
  QueryTeleClient fQtc;
  oam::OamCache* fOamCache = nullptr;
  bool fConcurrentSupport;
  uint32_t* fDebugLevel{nullptr};
};

}  // namespace

namespace ddlprocessor
{
DDLProcessor::DDLProcessor(int packageMaxThreads, int packageWorkQueueSize)
 : fPackageMaxThreads(packageMaxThreads), fPackageWorkQueueSize(packageWorkQueueSize), fMqServer(DDLProcName)
{
  fDdlPackagepool.setMaxThreads(fPackageMaxThreads);
  fDdlPackagepool.setQueueSize(fPackageWorkQueueSize);
  fDdlPackagepool.setName("DdlPackagepool");
  csc = CalpontSystemCatalog::makeCalpontSystemCatalog();
  csc->identity(CalpontSystemCatalog::EC);
  string teleServerHost(config::Config::makeConfig()->getConfig("QueryTele", "Host"));

  if (!teleServerHost.empty())
  {
    int teleServerPort =
        config::Config::fromText(config::Config::makeConfig()->getConfig("QueryTele", "Port"));

    if (teleServerPort > 0)
    {
      fQtc.serverParms(QueryTeleServerParms(teleServerHost, teleServerPort));
    }
  }
}

DDLProcessor::~DDLProcessor()
{
}
void DDLProcessor::process()
{
  DBRM dbrm;
  messageqcpp::IOSocket ios;
  bool concurrentSupport = true;
  string concurrentTranStr =
      config::Config::makeConfig()->getConfig("SystemConfig", "ConcurrentTransactions");

  if (concurrentTranStr.length() != 0)
  {
    if ((concurrentTranStr.compare("N") == 0) || (concurrentTranStr.compare("n") == 0))
      concurrentSupport = false;
  }

  cout << "DDLProc is ready..." << endl;

  try
  {
    for (;;)
    {
      try
      {
        ios = fMqServer.accept();
        fDdlPackagepool.invoke(PackageHandler(fQtc, &dbrm, ios, concurrentSupport, &debugLevel));
      }
      catch (...)
      {
        throw;
      }
    }
  }
  catch (exception& ex)
  {
    cerr << ex.what() << endl;
    messageqcpp::ByteStream results;
    messageqcpp::ByteStream::byte status = DDLPackageProcessor::NOT_ACCEPTING_PACKAGES;

    results << status;
    results << ex.what();
    ios.write(results);

    ios.close();
  }
  catch (...)
  {
    cerr << "Caught unknown exception!" << endl;
    messageqcpp::ByteStream results;
    messageqcpp::ByteStream::byte status = DDLPackageProcessor::NOT_ACCEPTING_PACKAGES;

    results << status;
    results << "Caught unknown exception!";
    ios.write(results);

    ios.close();
  }

  // wait for all the threads to exit
  fDdlPackagepool.wait();
}

int DDLProcessor::commitTransaction(uint32_t txnID, std::string& errorMsg)
{
  fWEClient = new WriteEngine::WEClients(WriteEngine::WEClients::DDLPROC);
  fPMCount = fWEClient->getPmCount();
  ByteStream bytestream;
  DBRM dbrm;
  uint64_t uniqueId = dbrm.getUnique64();
  fWEClient->addQueue(uniqueId);
  bytestream << (ByteStream::byte)WE_SVR_COMMIT_VERSION;
  bytestream << uniqueId;
  bytestream << txnID;
  uint32_t msgRecived = 0;
  fWEClient->write_to_all(bytestream);
  boost::shared_ptr<messageqcpp::ByteStream> bsIn;
  bsIn.reset(new ByteStream());
  int rc = 0;
  ByteStream::byte tmp8;

  while (1)
  {
    if (msgRecived == fPMCount)
      break;

    fWEClient->read(uniqueId, bsIn);

    if (bsIn->length() == 0)  // read error
    {
      rc = 1;
      errorMsg = "DDL cannot communicate with WES";
      fWEClient->removeQueue(uniqueId);
      break;
    }
    else
    {
      *bsIn >> tmp8;
      rc = tmp8;

      if (rc != 0)
      {
        *bsIn >> errorMsg;
        fWEClient->removeQueue(uniqueId);
        break;
      }
      else
        msgRecived++;
    }
  }

  delete fWEClient;
  fWEClient = 0;
  return rc;
}
}  // namespace ddlprocessor
