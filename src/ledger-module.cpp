#include "ledger-module.hpp"
#include "nack.hpp"
#include "dag/interlock-policy-descendants.hpp"
#include "dag/payload-map.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/string-helper.hpp>

namespace cledger::ledger {

NDN_LOG_INIT(cledger.ledger);

LedgerModule::LedgerModule(ndn::Face& face, ndn::KeyChain& keyChain, const std::string& configPath)
  : m_face(face)
  , m_keyChain(keyChain)
{
  // load the config and create storage
  m_config.load(configPath);
  m_validator.load(m_config.schemaFile);
  registerPrefix(); 
  
  Name topic = Name(m_config.ledgerPrefix).append("LEDGER").append("append");

  // initiliaze CA facing prefixes
  m_appendCt = std::make_unique<append::Ledger>(m_config.ledgerPrefix, topic, m_face, m_keyChain, m_validator);
  m_appendCt->listen(std::bind(&LedgerModule::onDataSubmission, this, _1));

  // initialize backend storage module
  m_storage = storage::LedgerStorage::createLedgerStorage(m_config.storageType, m_config.ledgerPrefix, "");

  // dag engine
  m_policy = dag::policy::InterlockPolicy::createInterlockPolicy(m_config.policyType, "");
  m_dag = std::make_unique<dag::DagModule>(m_storage->getInterface(), m_policy->getInterface());

  // initialize sync module
  Name syncPrefix = Name(m_config.ledgerPrefix).append("LEDGER").append("SYNC");
  m_syncOps.prefix = syncPrefix;
  m_syncOps.id = Name(m_config.ledgerPrefix).append(m_config.instanceSuffix);
  m_secOps.interestSigner->signingInfo = m_config.interestSigner;
  m_secOps.dataSigner->signingInfo = m_config.dataSigner;
  m_sync = std::make_unique<sync::SyncModule>(m_syncOps, m_secOps, m_face,
    m_storage->getInterface(),
    [this] (const Record& record) {
      if (record.getType() != tlv::REPLY_RECORD) {
        try {
          addPayloadMap(record.getPayload(), m_dag->add(record));
        }
        catch (const std::runtime_error& e) {
          NDN_LOG_TRACE("Adding PayloadMap failed because of: " << e.what());
        }
      }
      dagHarvest();
    }
  );

  // self publishing
  auto cert = m_keyChain.getPib().getIdentity(Name(m_config.ledgerPrefix).append(m_config.instanceSuffix))
                                 .getDefaultKey()
                                 .getDefaultCertificate();
  afterValidation(cert);
}

void
LedgerModule::registerPrefix()
{
  // register prefixes
  Name prefix = m_config.ledgerPrefix;
  // let's first use "LEDGER" in protocol
  prefix.append("LEDGER");
  // NDN_LOG_TRACE("here");
  auto prefixId = m_face.registerPrefix(
    prefix,
    [&] (const Name& name) {
      // register for each record Zone
      // notice: this only register FIB to Face, not NFD.
      for (auto& zone : m_config.recordZones) {
        auto filterId = m_face.setInterestFilter(zone, [this] (auto&&, const auto& i) { onQuery(i); });
        NDN_LOG_TRACE("Registering filter for recordZone " << zone);
        m_handle.handleFilter(filterId);
      }
    },
    [this] (auto&&, const auto& reason) { onRegisterFailed(reason); }
  );
  m_handle.handlePrefix(prefixId);
}

AppendStatus
LedgerModule::onDataSubmission(const Data& data)
{
  NDN_LOG_TRACE("Received Submission " << data);
  AppendStatus ret;
  m_validator.validate(data,
    [this, &data, &ret] (const Data&) {
      NDN_LOG_TRACE("Submitted Data conforms to trust schema");
      try {
        afterValidation(data);
        ret = AppendStatus::SUCCESS;
      }
      catch (std::exception& e) {
        NDN_LOG_TRACE("Submission failed because of: " << e.what());
        ret = AppendStatus::FAILURE_STORAGE;
      }
    },
    [&ret] (const Data&, const ndn::security::ValidationError& error) {
      NDN_LOG_ERROR("Error authenticating data: " << error);
      ret = AppendStatus::FAILURE_VALIDATION_APP;
    });
  return ret;
}

void
LedgerModule::onQuery(const Interest& query)
{
  // need to validate query format
  if (query.getForwardingHint().empty()) {
    // non-related, discard
    return;
  }

  NDN_LOG_TRACE("Received Query " << query); 
  auto interestName = query.getName();
  // interest for exact data match
  if (!query.getCanBePrefix())
  {
    // internal object name always start from /32=
    replyOrSendNack(interestName);
  }
  // query of a certificate or record
  else if (Certificate::isValidName(interestName.getPrefix(-1)) &&
           interestName.get(-1).toUri() == "32=record")
  {
    NDN_LOG_TRACE("A Record Query for " << interestName.getPrefix(-1));
    // 1. get the cert data (payload) with cert name
    // 2. map cert data to edge state name
    try {
      Block content(ndn::tlv::Content);
      auto payloadblock = m_storage->getBlock(interestName.getPrefix(-1));
      auto mapName = dag::toMapName(make_span<const uint8_t>(payloadblock.wire(), payloadblock.size()));

      NDN_LOG_TRACE("Finding PayloadMap... " << mapName);
      auto mapblock = m_storage->getBlock(mapName);
      auto payloadMap = dag::decodePayloadMap(mapblock);

      NDN_LOG_TRACE("Finding EdgeState... " << payloadMap.mapTo);
      auto stateblock = m_storage->getBlock(payloadMap.mapTo);
      auto state = dag::decodeEdgeState(stateblock);
      auto encoder = [this] (Block& b, const Name& n) {
        auto tlv = m_storage->getBlock(n);
        b.push_back(tlv);
      };

      // the queried record
      NDN_LOG_TRACE("Finding Record... " << dag::fromStateName(state.stateName));
      encoder(content, dag::fromStateName(state.stateName));
      
      // the descendants record
      ssize_t count = 0;
      for (auto& des : m_policy->select(state)) {
        if (count++ < m_config.policyThreshold) {
          NDN_LOG_TRACE("Finding Descendant Record... " << dag::fromStateName(des));
          encoder(content, dag::fromStateName(state.stateName));
        }
      }
      
      Name dataName(query.getName());
      dataName.append("data").appendTimestamp();
      Data data(dataName);
      // this deserves some considerations
      data.setFreshnessPeriod(m_config.nackFreshnessPeriod);
      data.setContent(content);
      m_keyChain.sign(data, signingByIdentity(Name(m_config.ledgerPrefix).append(m_config.instanceSuffix)));
      NDN_LOG_TRACE("Ledger replies with: " << data.getName());
      m_face.put(data);
    }
    catch (const std::exception& e) {
      NDN_LOG_DEBUG("Ledger storage cannot get the Data for reason: " << e.what());
      sendNack(query.getName());
    }
  }
  else {
    try {
      Data data(m_storage->getBlock(query.getName()));
      NDN_LOG_TRACE("Ledger replies with: " << data.getName());
      m_face.put(data);
    }
    catch (std::exception& e) {
      NDN_LOG_DEBUG("Ledger storage cannot get the Data for reason: " << e.what());
      // reply with app layer nack
      Nack nack;
      auto data = nack.prepareData(query.getName(), time::toUnixTimestamp(time::system_clock::now()));
      data->setFreshnessPeriod(m_config.nackFreshnessPeriod);
      m_keyChain.sign(*data, signingByIdentity(Name(m_config.ledgerPrefix).append(m_config.instanceSuffix)));
      NDN_LOG_TRACE("Ledger replies with: " << data->getName());
      m_face.put(*data);
    }
  }
}

// there may exist some race conditions, but in most cases they won't happen
void
LedgerModule::BackoffAndReply(std::chrono::milliseconds time)
{
  std::this_thread::sleep_for(time);

  Record newReply;
  newReply.setType(tlv::REPLY_RECORD);

  auto nonInterlocked = m_dag->harvest(m_config.policyThreshold, false);
  // two conditions: 1/ not a reply record; 2/ I haven't directly replied before
  for (auto& record : nonInterlocked) {
    if (m_repliedRecords.find(record.getName()) == m_repliedRecords.end() &&
        record.getType() != tlv::REPLY_RECORD) {
      newReply.addPointer(record.getName());
      m_repliedRecords.insert(record.getName());
    }
  }

  // publish records only if the reply record has pointers
  if (newReply.getPointers().size() > 1) {
    Name newReplyName = m_sync->publishRecord(newReply);
    newReply.setName(newReplyName);
    // add to DAG
    NDN_LOG_DEBUG("Generating new [Reply] Record " << newReply.getName());
    m_dag->add(newReply);
    dagHarvest();
  }
}

void
LedgerModule::onRegisterFailed(const std::string& reason)
{
  NDN_LOG_ERROR("Failed to register prefix in local hub's daemon, REASON: " << reason);
}

void
LedgerModule::afterValidation(const Data& data)
{
  NDN_LOG_TRACE("Receiving validated Data " << data.getName());
  // put raw data into storage
  try {
    m_storage->addBlock(data.getName(), data.wireEncode());
  }
  catch (const std::runtime_error& e) {
    NDN_LOG_DEBUG("Duplicate Data " << data.getName());
    return;
  }

  // it is either a certificate or revocation record
  Record newRecord;
  std::list<Name> pointers;
  // name is given by SVS, don't set it

  auto dataTlv = data.wireEncode();
  newRecord.setPayload(make_span<const uint8_t>(dataTlv.wire(), dataTlv.size()));
  for (auto& i : m_dag->getWaitList(0)) {
    NDN_LOG_DEBUG("Referencing to [Generic] " << dag::fromStateName(i));
    pointers.push_back(dag::fromStateName(i));
  }
  
  if (pointers.size() < 1) {
    // no waitlist, make a gensis record only referencing to itself
    newRecord.setType(tlv::GENESIS_RECORD);
    // a gensis record must be its first SVS publication,
    // therefore we can guess the name
    auto genesisName = m_sync->getSyncBase()->getMyDataName(1);
    NDN_LOG_DEBUG("Referencing to [Genesis] " << genesisName);
    newRecord.setPointers({genesisName});
  }
  else {
    // a generic record, add pointers normally
    newRecord.setType(tlv::GENERIC_RECORD);
    newRecord.setPointers(pointers);
  }
  // publish record
  Name name = m_sync->publishRecord(newRecord);
  newRecord.setName(name);
  // add to DAG
  NDN_LOG_DEBUG("Generating new Record " << newRecord.getName());
  addPayloadMap(newRecord.getPayload(), m_dag->add(newRecord));
  dagHarvest();
}

void
LedgerModule::addPayloadMap(const span<const uint8_t>& payload, const Name& mapTo)
{
  dag::PayloadMap map;
  map.mapName = dag::toMapName(payload);
  map.mapTo = mapTo;
  m_storage->addBlock(map.mapName, dag::encodePayloadMap(map));
}

Name
LedgerModule::getPayloadMap(const span<const uint8_t>& payload)
{
  dag::PayloadMap map;
  auto block = m_storage->getBlock(dag::toMapName(payload));
  return dag::decodePayloadMap(block).mapTo;
}

void
LedgerModule::sendNack(const Name& name)
{
  // reply with app layer nack
  Nack nack;
  auto data = nack.prepareData(name, time::toUnixTimestamp(time::system_clock::now()));
  data->setFreshnessPeriod(m_config.nackFreshnessPeriod);
  m_keyChain.sign(*data, signingByIdentity(Name(m_config.ledgerPrefix).append(m_config.instanceSuffix)));
  NDN_LOG_TRACE("Ledger replies with: " << data->getName());
  m_face.put(*data);
}

void
LedgerModule::replyOrSendNack(const Name& name)
{
  NDN_LOG_TRACE("Reply or Nack... " << name);
  try {
    Data data(m_storage->getBlock(name));
    NDN_LOG_TRACE("Ledger replies with: " << data.getName());
    m_face.put(data);
  }
  catch (std::exception& e) {
    sendNack(name);
  } 
}

void
LedgerModule::dagHarvest()
{
  // harvest record that collects enough citations (e.g., 3)
  // this ensures the waitlist be relatively small
  auto recordList = m_dag->harvest(m_config.policyThreshold, true);
  // put those record into 
  if (recordList.size() > 0) {
    NDN_LOG_DEBUG("The following Records have been interlocked");
    for (auto& r : recordList) {
      NDN_LOG_DEBUG("   " << r.getName());
      // if applicable, remove from the replied set
      if (m_repliedRecords.find(r.getName()) != m_repliedRecords.end()) {
        m_repliedRecords.erase(r.getName());
      }
    }
  }
}
} // namespace cledger::ledger
