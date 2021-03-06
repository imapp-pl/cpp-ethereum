/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file TestHelper.cpp
 * @author Marko Simovic <markobarko@gmail.com>
 * @date 2014
 */

#include "TestHelper.h"

#include <thread>
#include <chrono>
#include <libethereum/Client.h>
#include <liblll/Compiler.h>
#include <libevm/VMFactory.h>
#include "Stats.h"

using namespace std;
using namespace dev::eth;

namespace dev
{
namespace eth
{

void mine(Client& c, int numBlocks)
{
	auto startBlock = c.blockChain().details().number;

	c.startMining();
	while(c.blockChain().details().number < startBlock + numBlocks)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	c.stopMining();
}

void connectClients(Client& c1, Client& c2)
{
	(void)c1;
	(void)c2;
	// TODO: Move to WebThree. eth::Client no longer handles networking.
#if 0
	short c1Port = 20000;
	short c2Port = 21000;
	c1.startNetwork(c1Port);
	c2.startNetwork(c2Port);
	c2.connect("127.0.0.1", c1Port);
#endif
}

void mine(State& s, BlockChain const& _bc)
{
	s.commitToMine(_bc);
	GenericFarm<ProofOfWork> f;
	bool completed = false;
	f.onSolutionFound([&](ProofOfWork::Solution sol)
	{
		return completed = s.completeMine<ProofOfWork>(sol);
	});
	f.setWork(s.info());
	f.startCPU();
	while (!completed)
		this_thread::sleep_for(chrono::milliseconds(20));
}

void mine(BlockInfo& _bi)
{
	GenericFarm<ProofOfWork> f;
	bool completed = false;
	f.onSolutionFound([&](ProofOfWork::Solution sol)
	{
		ProofOfWork::assignResult(sol, _bi);
		return completed = true;
	});
	f.setWork(_bi);
	f.startCPU();
	while (!completed)
		this_thread::sleep_for(chrono::milliseconds(20));
}

}

namespace test
{

struct ValueTooLarge: virtual Exception {};
struct MissingFields : virtual Exception {};

bigint const c_max256plus1 = bigint(1) << 256;

ImportTest::ImportTest(json_spirit::mObject& _o, bool isFiller):
	m_statePre(OverlayDB(), eth::BaseState::Empty, Address(_o["env"].get_obj()["currentCoinbase"].get_str())),
	m_statePost(OverlayDB(), eth::BaseState::Empty, Address(_o["env"].get_obj()["currentCoinbase"].get_str())),
	m_TestObject(_o)
{
	importEnv(_o["env"].get_obj());
	importState(_o["pre"].get_obj(), m_statePre);
	importTransaction(_o["transaction"].get_obj());

	if (!isFiller)
	{
		importState(_o["post"].get_obj(), m_statePost);
		m_environment.sub.logs = importLog(_o["logs"].get_array());
	}
}

json_spirit::mObject& ImportTest::makeAllFieldsHex(json_spirit::mObject& _o)
{
	static const set<string> hashes {"bloom" , "coinbase", "hash", "mixHash", "parentHash", "receiptTrie",
									 "stateRoot", "transactionsTrie", "uncleHash", "currentCoinbase",
									 "previousHash", "to", "address", "caller", "origin", "secretKey", "data"};

	for (auto& i: _o)
	{
		std::string key = i.first;
		if (hashes.count(key))
			continue;

		std::string str;
		json_spirit::mValue value = i.second;

		if (value.type() == json_spirit::int_type)
			str = toString(value.get_int());
		else if (value.type() == json_spirit::str_type)
			str = value.get_str();
		else continue;

		_o[key] = (str.substr(0, 2) == "0x") ? str : toCompactHex(toInt(str), HexPrefix::Add, 1);
	}
	return _o;
}

void ImportTest::importEnv(json_spirit::mObject& _o)
{
	assert(_o.count("previousHash") > 0);
	assert(_o.count("currentGasLimit") > 0);
	assert(_o.count("currentDifficulty") > 0);
	assert(_o.count("currentTimestamp") > 0);
	assert(_o.count("currentCoinbase") > 0);
	assert(_o.count("currentNumber") > 0);

	m_environment.currentBlock.parentHash = h256(_o["previousHash"].get_str());
	m_environment.currentBlock.number = toInt(_o["currentNumber"]);
	m_environment.currentBlock.gasLimit = toInt(_o["currentGasLimit"]);
	m_environment.currentBlock.difficulty = toInt(_o["currentDifficulty"]);
	m_environment.currentBlock.timestamp = toInt(_o["currentTimestamp"]);
	m_environment.currentBlock.coinbaseAddress = Address(_o["currentCoinbase"].get_str());

	m_statePre.m_previousBlock = m_environment.previousBlock;
	m_statePre.m_currentBlock = m_environment.currentBlock;
}

// import state from not fully declared json_spirit::mObject, writing to _stateOptionsMap which fields were defined in json

void ImportTest::importState(json_spirit::mObject& _o, State& _state, stateOptionsMap& _stateOptionsMap)
{
	for (auto& i: _o)
	{
		json_spirit::mObject o = i.second.get_obj();

		ImportStateOptions stateOptions;
		u256 balance = 0;
		u256 nonce = 0;

		if (o.count("balance") > 0)
		{
			stateOptions.m_bHasBalance = true;
			if (bigint(o["balance"].get_str()) >= c_max256plus1)
				BOOST_THROW_EXCEPTION(ValueTooLarge() << errinfo_comment("State 'balance' is equal or greater than 2**256") );
			balance = toInt(o["balance"]);
		}

		if (o.count("nonce") > 0)
		{
			stateOptions.m_bHasNonce = true;
			if (bigint(o["nonce"].get_str()) >= c_max256plus1)
				BOOST_THROW_EXCEPTION(ValueTooLarge() << errinfo_comment("State 'nonce' is equal or greater than 2**256") );
			nonce = toInt(o["nonce"]);
		}

		Address address = Address(i.first);

		bytes code;
		if (o.count("code") > 0)
		{
			code = importCode(o);
			stateOptions.m_bHasCode = true;
		}

		if (code.size())
		{
			_state.m_cache[address] = Account(balance, Account::ContractConception);
			_state.m_cache[address].setCode(code);
		}
		else
			_state.m_cache[address] = Account(balance, Account::NormalCreation);

		if (o.count("storage") > 0)
		{
			stateOptions.m_bHasStorage = true;
			for (auto const& j: o["storage"].get_obj())
				_state.setStorage(address, toInt(j.first), toInt(j.second));
		}

		for (int i = 0; i < nonce; ++i)
			_state.noteSending(address);

		_state.ensureCached(address, false, false);
		_stateOptionsMap[address] = stateOptions;
	}
}

void ImportTest::importState(json_spirit::mObject& _o, State& _state)
{
	stateOptionsMap importedMap;
	importState(_o, _state, importedMap);
	for (auto& stateOptionMap : importedMap)
	{
		//check that every parameter was declared in state object
		if (!stateOptionMap.second.isAllSet())
			BOOST_THROW_EXCEPTION(MissingFields() << errinfo_comment("Import State: Missing state fields!"));	
	}
}

void ImportTest::importTransaction(json_spirit::mObject& _o)
{	
	if (_o.count("secretKey") > 0)
	{
		assert(_o.count("nonce") > 0);
		assert(_o.count("gasPrice") > 0);
		assert(_o.count("gasLimit") > 0);
		assert(_o.count("to") > 0);
		assert(_o.count("value") > 0);
		assert(_o.count("data") > 0);

		if (bigint(_o["nonce"].get_str()) >= c_max256plus1)
			BOOST_THROW_EXCEPTION(ValueTooLarge() << errinfo_comment("Transaction 'nonce' is equal or greater than 2**256") );
		if (bigint(_o["gasPrice"].get_str()) >= c_max256plus1)
			BOOST_THROW_EXCEPTION(ValueTooLarge() << errinfo_comment("Transaction 'gasPrice' is equal or greater than 2**256") );
		if (bigint(_o["gasLimit"].get_str()) >= c_max256plus1)
			BOOST_THROW_EXCEPTION(ValueTooLarge() << errinfo_comment("Transaction 'gasLimit' is equal or greater than 2**256") );
		if (bigint(_o["value"].get_str()) >= c_max256plus1)
			BOOST_THROW_EXCEPTION(ValueTooLarge() << errinfo_comment("Transaction 'value' is equal or greater than 2**256") );

		m_transaction = _o["to"].get_str().empty() ?
			Transaction(toInt(_o["value"]), toInt(_o["gasPrice"]), toInt(_o["gasLimit"]), importData(_o), toInt(_o["nonce"]), Secret(_o["secretKey"].get_str())) :
			Transaction(toInt(_o["value"]), toInt(_o["gasPrice"]), toInt(_o["gasLimit"]), Address(_o["to"].get_str()), importData(_o), toInt(_o["nonce"]), Secret(_o["secretKey"].get_str()));
	}
	else
	{
		RLPStream transactionRLPStream = createRLPStreamFromTransactionFields(_o);
		RLP transactionRLP(transactionRLPStream.out());
		m_transaction = Transaction(transactionRLP.data(), CheckTransaction::Everything);
	}
}

void ImportTest::checkExpectedState(State const& _stateExpect, State const& _statePost, stateOptionsMap const _expectedStateOptions, WhenError _throw)
{
	#define CHECK(a,b)						\
		{									\
			if (_throw == WhenError::Throw) \
				BOOST_CHECK_MESSAGE(a,b);	\
			else							\
				BOOST_WARN_MESSAGE(a,b);	\
		}

	for (auto const& a: _stateExpect.addresses())
	{
		CHECK(_statePost.addressInUse(a.first), "Filling Test: " << a.first << " missing expected address!");
		if (_statePost.addressInUse(a.first))
		{
			ImportStateOptions addressOptions(true);
			if(_expectedStateOptions.size())
			{
				try
				{
					addressOptions = _expectedStateOptions.at(a.first);
				}
				catch(std::out_of_range const&)
				{
					BOOST_ERROR("expectedStateOptions map does not match expectedState in checkExpectedState!");
					break;
				}
			}

			if (addressOptions.m_bHasBalance)
				CHECK(_stateExpect.balance(a.first) == _statePost.balance(a.first),
						"Check State: " << a.first <<  ": incorrect balance " << _statePost.balance(a.first) << ", expected " << _stateExpect.balance(a.first));

			if (addressOptions.m_bHasNonce)
				CHECK(_stateExpect.transactionsFrom(a.first) == _statePost.transactionsFrom(a.first),
						"Check State: " << a.first <<  ": incorrect nonce " << _statePost.transactionsFrom(a.first) << ", expected " << _stateExpect.transactionsFrom(a.first));

			if (addressOptions.m_bHasStorage)
			{
				map<u256, u256> stateStorage = _statePost.storage(a.first);
				for (auto const& s: _stateExpect.storage(a.first))
					CHECK(stateStorage[s.first] == s.second,
							"Check State: " << a.first <<  ": incorrect storage [" << s.first << "] = " << toHex(stateStorage[s.first]) << ", expected [" << s.first << "] = " << toHex(s.second));

				//Check for unexpected storage values
				stateStorage = _stateExpect.storage(a.first);
				for (auto const& s: _statePost.storage(a.first))
					CHECK(stateStorage[s.first] == s.second,
							"Check State: " << a.first <<  ": incorrect storage [" << s.first << "] = " << toHex(s.second) << ", expected [" << s.first << "] = " << toHex(stateStorage[s.first]));
			}

			if (addressOptions.m_bHasCode)
				CHECK(_stateExpect.code(a.first) == _statePost.code(a.first),
						"Check State: " << a.first <<  ": incorrect code '" << toHex(_statePost.code(a.first)) << "', expected '" << toHex(_stateExpect.code(a.first)) << "'");
		}
	}
}

void ImportTest::exportTest(bytes const& _output, State const& _statePost)
{
	// export output
	m_TestObject["out"] = toHex(_output, 2, HexPrefix::Add);

	// export logs
	m_TestObject["logs"] = exportLog(_statePost.pending().size() ? _statePost.log(0) : LogEntries());

	// compare expected state with post state
	if (m_TestObject.count("expect") > 0)
	{
		stateOptionsMap stateMap;
		State expectState(OverlayDB(), eth::BaseState::Empty);
		importState(m_TestObject["expect"].get_obj(), expectState, stateMap);
		checkExpectedState(expectState, _statePost, stateMap, Options::get().checkState ? WhenError::Throw : WhenError::DontThrow);
		m_TestObject.erase(m_TestObject.find("expect"));
	}

	// export post state
	m_TestObject["post"] = fillJsonWithState(_statePost);
	m_TestObject["postStateRoot"] = toHex(_statePost.rootHash().asBytes());

	// export pre state
	m_TestObject["pre"] = fillJsonWithState(m_statePre);
	m_TestObject["env"] = makeAllFieldsHex(m_TestObject["env"].get_obj());
	m_TestObject["transaction"] = makeAllFieldsHex(m_TestObject["transaction"].get_obj());
}

json_spirit::mObject fillJsonWithTransaction(Transaction _txn)
{
	json_spirit::mObject txObject;
	txObject["nonce"] = toCompactHex(_txn.nonce(), HexPrefix::Add, 1);
	txObject["data"] = toHex(_txn.data(), 2, HexPrefix::Add);
	txObject["gasLimit"] = toCompactHex(_txn.gas(), HexPrefix::Add, 1);
	txObject["gasPrice"] = toCompactHex(_txn.gasPrice(), HexPrefix::Add, 1);
	txObject["r"] = toCompactHex(_txn.signature().r, HexPrefix::Add, 1);
	txObject["s"] = toCompactHex(_txn.signature().s, HexPrefix::Add, 1);
	txObject["v"] = toCompactHex(_txn.signature().v + 27, HexPrefix::Add, 1);
	txObject["to"] = _txn.isCreation() ? "" : toString(_txn.receiveAddress());
	txObject["value"] = toCompactHex(_txn.value(), HexPrefix::Add, 1);
	return txObject;
}

json_spirit::mObject fillJsonWithState(State _state)
{
	json_spirit::mObject oState;
	for (auto const& a: _state.addresses())
	{
		json_spirit::mObject o;
		o["balance"] = toCompactHex(_state.balance(a.first), HexPrefix::Add, 1);
		o["nonce"] = toCompactHex(_state.transactionsFrom(a.first), HexPrefix::Add, 1);
		{
			json_spirit::mObject store;
			for (auto const& s: _state.storage(a.first))
				store[toCompactHex(s.first, HexPrefix::Add, 1)] = toCompactHex(s.second, HexPrefix::Add, 1);
			o["storage"] = store;
		}
		o["code"] = toHex(_state.code(a.first), 2, HexPrefix::Add);
		oState[toString(a.first)] = o;
	}
	return oState;
}

json_spirit::mArray exportLog(eth::LogEntries _logs)
{
	json_spirit::mArray ret;
	if (_logs.size() == 0) return ret;
	for (LogEntry const& l: _logs)
	{
		json_spirit::mObject o;
		o["address"] = toString(l.address);
		json_spirit::mArray topics;
		for (auto const& t: l.topics)
			topics.push_back(toString(t));
		o["topics"] = topics;
		o["data"] = toHex(l.data, 2, HexPrefix::Add);
		o["bloom"] = toString(l.bloom());
		ret.push_back(o);
	}
	return ret;
}

u256 toInt(json_spirit::mValue const& _v)
{
	switch (_v.type())
	{
	case json_spirit::str_type: return u256(_v.get_str());
	case json_spirit::int_type: return (u256)_v.get_uint64();
	case json_spirit::bool_type: return (u256)(uint64_t)_v.get_bool();
	case json_spirit::real_type: return (u256)(uint64_t)_v.get_real();
	default: cwarn << "Bad type for scalar: " << _v.type();
	}
	return 0;
}

byte toByte(json_spirit::mValue const& _v)
{
	switch (_v.type())
	{
	case json_spirit::str_type: return (byte)stoi(_v.get_str());
	case json_spirit::int_type: return (byte)_v.get_uint64();
	case json_spirit::bool_type: return (byte)_v.get_bool();
	case json_spirit::real_type: return (byte)_v.get_real();
	default: cwarn << "Bad type for scalar: " << _v.type();
	}
	return 0;
}

bytes importByteArray(std::string const& _str)
{
	return fromHex(_str.substr(0, 2) == "0x" ? _str.substr(2) : _str, WhenError::Throw);
}

bytes importData(json_spirit::mObject& _o)
{
	bytes data;
	if (_o["data"].type() == json_spirit::str_type)
		data = importByteArray(_o["data"].get_str());
	else
		for (auto const& j: _o["data"].get_array())
			data.push_back(toByte(j));
	return data;
}

bytes importCode(json_spirit::mObject& _o)
{
	bytes code;
	if (_o["code"].type() == json_spirit::str_type)
		if (_o["code"].get_str().find_first_of("0x") != 0)
			code = compileLLL(_o["code"].get_str(), false);
		else
			code = fromHex(_o["code"].get_str().substr(2));
	else if (_o["code"].type() == json_spirit::array_type)
	{
		code.clear();
		for (auto const& j: _o["code"].get_array())
			code.push_back(toByte(j));
	}
	return code;
}

LogEntries importLog(json_spirit::mArray& _a)
{
	LogEntries logEntries;
	for (auto const& l: _a)
	{
		json_spirit::mObject o = l.get_obj();
		// cant use BOOST_REQUIRE, because this function is used outside boost test (createRandomTest)
		assert(o.count("address") > 0);
		assert(o.count("topics") > 0);
		assert(o.count("data") > 0);
		assert(o.count("bloom") > 0);
		LogEntry log;
		log.address = Address(o["address"].get_str());
		for (auto const& t: o["topics"].get_array())
			log.topics.push_back(h256(t.get_str()));
		log.data = importData(o);
		logEntries.push_back(log);
	}
	return logEntries;
}

void checkOutput(bytes const& _output, json_spirit::mObject& _o)
{
	int j = 0;
	if (_o["out"].type() == json_spirit::array_type)
		for (auto const& d: _o["out"].get_array())
		{
			BOOST_CHECK_MESSAGE(_output[j] == toInt(d), "Output byte [" << j << "] different!");
			++j;
		}
	else if (_o["out"].get_str().find("0x") == 0)
		BOOST_CHECK(_output == fromHex(_o["out"].get_str().substr(2)));
	else
		BOOST_CHECK(_output == fromHex(_o["out"].get_str()));
}

void checkStorage(map<u256, u256> _expectedStore, map<u256, u256> _resultStore, Address _expectedAddr)
{
	for (auto&& expectedStorePair : _expectedStore)
	{
		auto& expectedStoreKey = expectedStorePair.first;
		auto resultStoreIt = _resultStore.find(expectedStoreKey);
		if (resultStoreIt == _resultStore.end())
			BOOST_ERROR(_expectedAddr << ": missing store key " << expectedStoreKey);
		else
		{
			auto& expectedStoreValue = expectedStorePair.second;
			auto& resultStoreValue = resultStoreIt->second;
			BOOST_CHECK_MESSAGE(expectedStoreValue == resultStoreValue, _expectedAddr << ": store[" << expectedStoreKey << "] = " << resultStoreValue << ", expected " << expectedStoreValue);
		}
	}
	BOOST_CHECK_EQUAL(_resultStore.size(), _expectedStore.size());
	for (auto&& resultStorePair: _resultStore)
	{
		if (!_expectedStore.count(resultStorePair.first))
			BOOST_ERROR(_expectedAddr << ": unexpected store key " << resultStorePair.first);
	}
}

void checkLog(LogEntries _resultLogs, LogEntries _expectedLogs)
{
	BOOST_REQUIRE_EQUAL(_resultLogs.size(), _expectedLogs.size());

	for (size_t i = 0; i < _resultLogs.size(); ++i)
	{
		BOOST_CHECK_EQUAL(_resultLogs[i].address, _expectedLogs[i].address);
		BOOST_CHECK_EQUAL(_resultLogs[i].topics, _expectedLogs[i].topics);
		BOOST_CHECK(_resultLogs[i].data == _expectedLogs[i].data);
	}
}

void checkCallCreates(eth::Transactions _resultCallCreates, eth::Transactions _expectedCallCreates)
{
	BOOST_REQUIRE_EQUAL(_resultCallCreates.size(), _expectedCallCreates.size());

	for (size_t i = 0; i < _resultCallCreates.size(); ++i)
	{
		BOOST_CHECK(_resultCallCreates[i].data() == _expectedCallCreates[i].data());
		BOOST_CHECK(_resultCallCreates[i].receiveAddress() == _expectedCallCreates[i].receiveAddress());
		BOOST_CHECK(_resultCallCreates[i].gas() == _expectedCallCreates[i].gas());
		BOOST_CHECK(_resultCallCreates[i].value() == _expectedCallCreates[i].value());
	}
}

void userDefinedTest(string testTypeFlag, std::function<void(json_spirit::mValue&, bool)> doTests)
{
	Options::get(); // parse command line options, e.g. to enable JIT

	for (int i = 1; i < boost::unit_test::framework::master_test_suite().argc; ++i)
	{
		string arg = boost::unit_test::framework::master_test_suite().argv[i];
		if (arg == testTypeFlag)
		{
			if (boost::unit_test::framework::master_test_suite().argc <= i + 2)
			{
				cnote << "Missing filename\nUsage: testeth " << testTypeFlag << " <filename> <testname>\n";
				return;
			}
			string filename = boost::unit_test::framework::master_test_suite().argv[i + 1];
			string testname = boost::unit_test::framework::master_test_suite().argv[i + 2];
			int currentVerbosity = g_logVerbosity;
			g_logVerbosity = 12;
			try
			{
				cnote << "Testing user defined test: " << filename;
				json_spirit::mValue v;
				string s = asString(contents(filename));
				BOOST_REQUIRE_MESSAGE(s.length() > 0, "Contents of " + filename + " is empty. ");
				json_spirit::read_string(s, v);
				json_spirit::mObject oSingleTest;

				json_spirit::mObject::const_iterator pos = v.get_obj().find(testname);
				if (pos == v.get_obj().end())
				{
					cnote << "Could not find test: " << testname << " in " << filename << "\n";
					return;
				}
				else
					oSingleTest[pos->first] = pos->second;

				json_spirit::mValue v_singleTest(oSingleTest);
				doTests(v_singleTest, false);
			}
			catch (Exception const& _e)
			{
				BOOST_ERROR("Failed Test with Exception: " << diagnostic_information(_e));
				g_logVerbosity = currentVerbosity;
			}
			catch (std::exception const& _e)
			{
				BOOST_ERROR("Failed Test with Exception: " << _e.what());
				g_logVerbosity = currentVerbosity;
			}
			g_logVerbosity = currentVerbosity;
		}
	}
}

void executeTests(const string& _name, const string& _testPathAppendix, const boost::filesystem::path _pathToFiller, std::function<void(json_spirit::mValue&, bool)> doTests)
{
	string testPath = getTestPath();
	testPath += _testPathAppendix;

	if (Options::get().stats)
		Listener::registerListener(Stats::get());

	if (Options::get().fillTests)
	{
		try
		{
			cnote << "Populating tests...";
			json_spirit::mValue v;
			boost::filesystem::path p(__FILE__);
			string s = asString(dev::contents(_pathToFiller.string() + "/" + _name + "Filler.json"));
			BOOST_REQUIRE_MESSAGE(s.length() > 0, "Contents of " + _pathToFiller.string() + "/" + _name + "Filler.json is empty.");
			json_spirit::read_string(s, v);
			doTests(v, true);
			writeFile(testPath + "/" + _name + ".json", asBytes(json_spirit::write_string(v, true)));
		}
		catch (Exception const& _e)
		{
			BOOST_ERROR("Failed filling test with Exception: " << diagnostic_information(_e));
		}
		catch (std::exception const& _e)
		{
			BOOST_ERROR("Failed filling test with Exception: " << _e.what());
		}
	}

	try
	{
		std::cout << "TEST " << _name << ":\n";
		json_spirit::mValue v;
		string s = asString(dev::contents(testPath + "/" + _name + ".json"));
		BOOST_REQUIRE_MESSAGE(s.length() > 0, "Contents of " + testPath + "/" + _name + ".json is empty. Have you cloned the 'tests' repo branch develop and set ETHEREUM_TEST_PATH to its path?");
		json_spirit::read_string(s, v);
		Listener::notifySuiteStarted(_name);
		doTests(v, false);
	}
	catch (Exception const& _e)
	{
		BOOST_ERROR("Failed test with Exception: " << diagnostic_information(_e));
	}
	catch (std::exception const& _e)
	{
		BOOST_ERROR("Failed test with Exception: " << _e.what());
	}
}

RLPStream createRLPStreamFromTransactionFields(json_spirit::mObject& _tObj)
{
	//Construct Rlp of the given transaction
	RLPStream rlpStream;
	rlpStream.appendList(_tObj.size());

	if (_tObj.count("nonce"))
		rlpStream << bigint(_tObj["nonce"].get_str());

	if (_tObj.count("gasPrice"))
		rlpStream << bigint(_tObj["gasPrice"].get_str());

	if (_tObj.count("gasLimit"))
		rlpStream << bigint(_tObj["gasLimit"].get_str());

	if (_tObj.count("to"))
	{
		if (_tObj["to"].get_str().empty())
			rlpStream << "";
		else
			rlpStream << importByteArray(_tObj["to"].get_str());
	}

	if (_tObj.count("value"))
		rlpStream << bigint(_tObj["value"].get_str());

	if (_tObj.count("data"))
		rlpStream << importData(_tObj);

	if (_tObj.count("v"))
		rlpStream << bigint(_tObj["v"].get_str());

	if (_tObj.count("r"))
		rlpStream << bigint(_tObj["r"].get_str());

	if (_tObj.count("s"))
		rlpStream <<  bigint(_tObj["s"].get_str());

	if (_tObj.count("extrafield"))
		rlpStream << bigint(_tObj["extrafield"].get_str());

	return rlpStream;
}

Options::Options()
{
	auto argc = boost::unit_test::framework::master_test_suite().argc;
	auto argv = boost::unit_test::framework::master_test_suite().argv;

	for (auto i = 0; i < argc; ++i)
	{
		auto arg = std::string{argv[i]};
		if (arg == "--jit")
			eth::VMFactory::setKind(eth::VMKind::JIT);
		else if (arg == "--vm=smart")
			eth::VMFactory::setKind(eth::VMKind::Smart);
		else if (arg == "--vmtrace")
			vmtrace = true;
		else if (arg == "--filltests")
			fillTests = true;
		else if (arg.compare(0, 7, "--stats") == 0)
		{
			stats = true;
			if (arg.size() > 7)
				statsOutFile = arg.substr(8); // skip '=' char
		}
		else if (arg == "--performance")
			performance = true;
		else if (arg == "--quadratic")
			quadratic = true;
		else if (arg == "--memory")
			memory = true;
		else if (arg == "--inputlimits")
			inputLimits = true;
		else if (arg == "--bigdata")
			bigData = true;
		else if (arg == "--checkstate")
			checkState = true;
		else if (arg == "--all")
		{
			performance = true;
			quadratic = true;
			memory = true;
			inputLimits = true;
			bigData = true;
		}
	}
}

Options const& Options::get()
{
	static Options instance;
	return instance;
}


LastHashes lastHashes(u256 _currentBlockNumber)
{
	LastHashes ret;
	for (u256 i = 1; i <= 256 && i <= _currentBlockNumber; ++i)
		ret.push_back(sha3(toString(_currentBlockNumber - i)));
	return ret;
}


namespace
{
	Listener* g_listener;
}

void Listener::registerListener(Listener& _listener)
{
	g_listener = &_listener;
}

void Listener::notifySuiteStarted(std::string const& _name)
{
	if (g_listener)
		g_listener->suiteStarted(_name);
}

void Listener::notifyTestStarted(std::string const& _name)
{
	if (g_listener)
		g_listener->testStarted(_name);
}

void Listener::notifyTestFinished()
{
	if (g_listener)
		g_listener->testFinished();
}

} } // namespaces
