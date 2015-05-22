#include <fstream>
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>
#include <boost/range.hpp>

#pragma warning(push)
#pragma warning(disable: 4100)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../json_spirit/json_spirit_reader_template.h"
#pragma GCC diagnostic pop
#pragma warning(pop)

#include <libdevcore/CommonIO.h>

using namespace boost::unit_test;
namespace fs = boost::filesystem;

namespace dev { namespace test {

void doVMTest(json_spirit::mObject& o, bool _fillin);

}}

bool loadTestDefs(fs::path const& _dir, test_suite& _parent)
{
	static std::vector<json_spirit::mValue> s_suiteDefs;

	auto r = boost::make_iterator_range(fs::directory_iterator{_dir}, fs::directory_iterator{});
	for (auto&& entry: r)
	{
		auto&& path = entry.path();
		if (fs::is_regular_file(path) && path.extension() == ".json")
		{
			// TODO: add blacklisting
			if (path.filename() == "vmInputLimits.json" || path.filename() == "vmInputLimitsLight.json")
				continue;
			std::cerr << "Loading " << path.filename() << "\n";
			auto suite = new test_suite{path.filename().string()};
			s_suiteDefs.emplace_back();
			auto& suiteDef = s_suiteDefs.back();
			std::ifstream suiteFile{path.string()};
			json_spirit::read_stream(suiteFile, suiteDef);
			for (auto&& d: suiteDef.get_obj())
			{
				auto& testName = d.first;
				auto& testDef = d.second.get_obj();
				auto testFunc = [&testDef]() { dev::test::doVMTest(testDef, false); };
				suite->add(make_test_case(testFunc, testName));
			}
			_parent.add(suite);
		}
		else if (fs::is_directory(path))
		{
			auto suite = new test_suite{path.filename().string()};
			_parent.add(suite);
			loadTestDefs(path, *suite);
		}
	}
	return true;
}

bool initVMTestSuite()
{
	auto& parentSuite = *new test_suite{"vm"};
	framework::master_test_suite().add(&parentSuite);
	return loadTestDefs({"/home/chfast/Projects/ethereum/tests/VMTests"}, parentSuite);
}
