/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <test/libsolidity/SemanticsTest.h>
#include <test/Options.h>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/throw_exception.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <stdexcept>

using namespace dev;
using namespace solidity;
using namespace dev::solidity::test;
using namespace dev::solidity::test::formatting;
using namespace std;
namespace fs = boost::filesystem;
using namespace boost;
using namespace boost::algorithm;
using namespace boost::unit_test;

SemanticsTest::SemanticsTest(string const& _filename):
	SolidityExecutionFramework(ipcPath)
{
	ifstream file(_filename);
	if (!file)
		BOOST_THROW_EXCEPTION(runtime_error("Cannot open test contract: \"" + _filename + "\"."));
	file.exceptions(ios::badbit);

	m_source = parseSource(file);
	m_calls = parseCalls(file);
}


bool SemanticsTest::run(ostream& _stream, string const& _linePrefix, bool const _formatted)
{
	compileAndRun(m_source);

	bool success = true;
	m_results.clear();
	for (auto const& test: m_calls)
	{
		m_results.emplace_back(callContractFunctionWithValueNoEncoding(
			test.signature,
			test.value,
			test.argumentBytes
		));

		if (m_results.back() != test.expectedBytes)
			success = false;
	}

	if (!success)
	{
		string nextIndentLevel = _linePrefix + "  ";
		FormattedScope(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Expected result:" << endl;
		printCalls(false, _stream, nextIndentLevel, _formatted);
		FormattedScope(_stream, _formatted, {BOLD, CYAN}) << _linePrefix << "Obtained result:" << endl;
		printCalls(true, _stream, nextIndentLevel, _formatted);
		return false;
	}
	return true;
}

void SemanticsTest::printContract(ostream& _stream, string const& _linePrefix, bool const) const
{
	stringstream stream(m_source);
	string line;
	while (getline(stream, line))
		_stream << _linePrefix << line << endl;
}

void SemanticsTest::printUpdatedExpectations(std::ostream& _stream, std::string const& _linePrefix) const
{
	printCalls(true, _stream, _linePrefix, false);
}

bool SemanticsTest::ByteRangeFormat::padsLeft() const
{
	switch (type)
	{
		case Bool:
		case Dec:
		case Hex:
		case SignedDec:
			return true;
		case Hash:
		case HexString:
		case String:
			return false;
		default:
			solAssert(false, "");
	}
}

std::string SemanticsTest::ByteRangeFormat::tryFormat(
	bytes::const_iterator _it,
	bytes::const_iterator _end
) const
{
	solAssert(_it < _end, "");
	solAssert(length != 0, "");

	if (padded)
	{
		size_t paddedLength = ((length + 31) & ~31);
		if (size_t(_end - _it) < paddedLength) return {};
		auto isZero = [](byte const& _v) -> bool { return _v == 0; };
		if (padsLeft())
		{
			if (!all_of(_it, _it + (paddedLength - length), isZero))
				return {};
			_it += paddedLength - length;
		}
		else if (!all_of(_it + length, _it + paddedLength, isZero))
			return {};
	}
	else if (size_t(_end - _it) < length)
		return {};

	bytes byteRange(_it, _it + length);
	stringstream result;
	switch(type)
	{
		case ByteRangeFormat::SignedDec:
			if (*_it & 0x80)
			{
				for (auto& v: byteRange)
					v ^= 0xFF;
				result << "-" << fromBigEndian<u256>(byteRange) + 1;
			}
			else
				result << fromBigEndian<u256>(byteRange);
			break;
		case ByteRangeFormat::Dec:
			result << fromBigEndian<u256>(byteRange);
			break;
		case ByteRangeFormat::Hash:
		case ByteRangeFormat::Hex:
			result << "0x" << hex << fromBigEndian<u256>(byteRange);
			break;
		case ByteRangeFormat::HexString:
			result << "hex\"" << toHex(byteRange) << "\"";
			break;
		case ByteRangeFormat::Bool:
		{
			auto val = fromBigEndian<u256>(byteRange);
			if (val == u256(1))
				result << "true";
			else if (val == u256(0))
				result << "false";
			else
				return {};
			break;
		}
		case ByteRangeFormat::String:
		{
			result << "\"";
			bool expectZeros = false;
			for (auto const& v: byteRange)
			{
				if (expectZeros && v != 0)
					return {};
				if (v == 0) expectZeros = true;
				else
				{
					if (!isprint(v) || v == '"')
						return {};
					result << v;
				}
			}
			result << "\"";
			break;
		}
	}

	return result.str();

}

SemanticsTest::ByteRangeFormat ChooseNextRangeFormat(bytes::const_iterator _start, bytes::const_iterator _end)
{
	// TODO: use some heuristic to choose a better format
	solAssert(_start < _end, "");
	size_t length = size_t(_end - _start);
	if (length >= 32)
		return {32, SemanticsTest::ByteRangeFormat::Hex, true};
	else
		return {length, SemanticsTest::ByteRangeFormat::HexString, false};
}

string SemanticsTest::bytesToString(
	bytes const& _bytes,
	vector<ByteRangeFormat> const& _formatList
)
{
	string result;

	auto it = _bytes.begin();
	bool padded = true;

	auto formatit = _formatList.begin();

	while(it != _bytes.end())
	{
		ByteRangeFormat format = (formatit == _formatList.end())?
			ChooseNextRangeFormat(it, _bytes.end()) : *formatit++;

		// check for end of unpadded block
		if (!padded && format.padded)
		{
			result += ")";
			padded = true;
		}

		if (it != _bytes.begin())
			result += ", ";

		// check for beginning of unpadded block
		if (padded && !format.padded)
		{
			result += "unpadded(";
			padded = false;
		}

		string formatted = format.tryFormat(it, _bytes.end());
		if (!formatted.empty())
		{
			result += formatted;
			if (format.padded)
				it += (format.length + 31) & ~31;
			else
				it += format.length;
		}
		else
			formatit = _formatList.end();
	}

	if (!padded)
		result += ")";

	solAssert(stringToBytes(result) == _bytes, "Conversion to string failed.");
	return result;
}

bytes SemanticsTest::stringToBytes(string _list, vector<ByteRangeFormat>* _formatList, bool padded)
{
	bytes result;
	auto it = _list.begin();
	while (it != _list.end())
	{
		if (isdigit(*it) || (*it == '-' && (it + 1) != _list.end() && isdigit(*(it + 1))))
		{
			bool isNegative = (*it == '-');

			auto valueBegin = it;
			while (it != _list.end() && !isspace(*it) && *it != ',')
				++it;

			bytes newBytes;
			u256 numberValue(string(valueBegin, it));
			if (padded)
				newBytes = toBigEndian(numberValue);
			else if (numberValue == u256(0))
				newBytes = bytes{0};
			else
				newBytes = toCompactBigEndian(numberValue);
			result += newBytes;
			if (_formatList)
			{
				ByteRangeFormat::Type type;
				// note that signed hex numbers will be parsed correctly,
				// but re-encoded as signed dec numbers
				if (isNegative)
					type = ByteRangeFormat::SignedDec;
				else if (*it == '0' && it + 1 != _list.end() && *(it + 1) == 'x')
					type = ByteRangeFormat::Hex;
				else
					type = ByteRangeFormat::Dec;
				_formatList->emplace_back(ByteRangeFormat{newBytes.size(), type, padded});
			}
		}
		else if (*it == '"')
		{
			++it;
			auto stringBegin = it;
			// TODO: handle escaped quotes, resp. escape sequences in general
			while (it != _list.end() && *it != '"')
				++it;
			bytes stringBytes = asBytes(string(stringBegin, it));
			expect(it, _list.end(), '"');

			result += stringBytes;
			if (padded)
				result += bytes((32 - stringBytes.size() % 32) % 32, 0);
			if (_formatList)
				_formatList->emplace_back(ByteRangeFormat{stringBytes.size(), ByteRangeFormat::String, padded});
		}
		else if (starts_with(iterator_range<string::iterator>(it, _list.end()), "keccak256("))
		{
			if (_formatList)
				_formatList->emplace_back(ByteRangeFormat{32, ByteRangeFormat::Hash, padded});

			it += 10; // skip "keccak256("

			unsigned int parenthesisLevel = 1;
			auto nestedListBegin = it;
			while (it != _list.end())
			{
				if (*it == '(') ++parenthesisLevel;
				else if (*it == ')')
				{
					--parenthesisLevel;
					if (parenthesisLevel == 0)
						break;
				}
				++it;
			}
			bytes nestedResult = stringToBytes(string(nestedListBegin, it));
			expect(it, _list.end(), ')');
			result += keccak256(nestedResult).asBytes();
		}
		else if (starts_with(iterator_range<string::iterator>(it, _list.end()), "hex\""))
		{
			it += 4; // skip "hex\""
			auto hexStringBegin = it;
			while (it != _list.end() && *it != '"')
				++it;
			string hexString(hexStringBegin, it);
			bytes hexBytes = fromHex(hexString);
			expect(it, _list.end(), '"');

			result += hexBytes;
			if (padded)
				result += bytes((32 - hexBytes.size() % 32) % 32, 0);
			if (_formatList)
				_formatList->emplace_back(ByteRangeFormat{hexBytes.size(), ByteRangeFormat::HexString, padded});
		}
		else if (starts_with(iterator_range<string::iterator>(it, _list.end()), "unpadded("))
		{
			it += 9; // skip "unpadded("

			unsigned int parenthesisLevel = 1;
			auto nestedListBegin = it;
			while (it != _list.end())
			{
				if (*it == '(') ++parenthesisLevel;
				else if (*it == ')')
				{
					--parenthesisLevel;
					if (parenthesisLevel == 0)
						break;
				}
				++it;
			}
			result += stringToBytes(string(nestedListBegin, it), _formatList, false);
			expect(it, _list.end(), ')');
		}
		else if (starts_with(iterator_range<string::iterator>(it, _list.end()), "true"))
		{
			it += 4; // skip "true"
			if (padded)
				result += bytes(31, 0);
			result += bytes{1};
			if (_formatList)
				_formatList->emplace_back(ByteRangeFormat{1, ByteRangeFormat::Bool, padded});
		}
		else if (starts_with(iterator_range<string::iterator>(it, _list.end()), "false"))
		{
			it += 5; // skip "false"
			if (padded)
				result += bytes(31, 0);
			result += bytes{0};
			if (_formatList)
				_formatList->emplace_back(ByteRangeFormat{1, ByteRangeFormat::Bool, padded});
		}
		else
			BOOST_THROW_EXCEPTION(runtime_error("Test expectations contain invalidly formatted data."));

		skipWhitespace(it, _list.end());
		if (it != _list.end())
			expect(it, _list.end(), ',');
		skipWhitespace(it, _list.end());
	}
	return result;
}

void SemanticsTest::printCalls(
	bool _actualResults,
	ostream& _stream,
	string const& _linePrefix,
	bool const _formatted
) const
{
	solAssert(m_calls.size() == m_results.size(), "");
	for (size_t i = 0; i < m_calls.size(); i++)
	{
		auto const& call = m_calls[i];
		_stream << _linePrefix << call.signature;
		if (call.value > u256(0))
			_stream << "[" << call.value << "]";
		if (!call.arguments.empty())
		{
			_stream << ": " << call.arguments;
		}
		_stream << endl;
		string result;
		std::vector<ByteRangeFormat> formatList;
		auto expectedBytes = stringToBytes(call.expectedResult, &formatList);
		if (_actualResults)
			result = bytesToString(m_results[i], formatList);
		else
			result = call.expectedResult;

		_stream << _linePrefix;
		if (_formatted && m_results[i] != expectedBytes)
			_stream << formatting::RED_BACKGROUND;
		if (result.empty())
			_stream << "REVERT";
		else
			_stream << "-> " << result;
		if (_formatted && m_results[i] != expectedBytes)
			_stream << formatting::RESET;
		_stream << endl;
	}
}

vector<SemanticsTest::SemanticsTestFunctionCall> SemanticsTest::parseCalls(istream& _stream)
{
	vector<SemanticsTestFunctionCall> expectations;
	string line;
	while (getline(_stream, line))
	{
		auto it = line.begin();

		skipSlashes(it, line.end());
		skipWhitespace(it, line.end());

		string arguments;
		bytes argumentBytes;
		u256 ether(0);
		string expectedResult;
		bytes expectedBytes;
		vector<ByteRangeFormat> expectedFormat;

		if (it == line.end())
			continue;

		auto signatureBegin = it;
		while (it != line.end() && *it != ')')
			++it;
		expect(it, line.end(), ')');

		string signature(signatureBegin, it);

		if (it != line.end() && *it == '[')
		{
			++it;
			auto etherBegin = it;
			while (it != line.end() && *it != ']')
				++it;
			string etherString(etherBegin, it);
			ether = u256(etherString);
			expect(it, line.end(), ']');
		}

		skipWhitespace(it, line.end());

		if (it != line.end())
		{
			expect(it, line.end(), ':');
			skipWhitespace(it, line.end());
			arguments = string(it, line.end());
			argumentBytes = stringToBytes(arguments);
		}

		if (!getline(_stream, line))
			throw runtime_error("Invalid test expectation. No result specified.");

		it = line.begin();
		skipSlashes(it, line.end());
		skipWhitespace(it, line.end());

		if (it != line.end() && *it == '-')
		{
			expect(it, line.end(), '-');
			expect(it, line.end(), '>');

			skipWhitespace(it, line.end());

			expectedResult = string(it, line.end());
			expectedBytes = stringToBytes(expectedResult, &expectedFormat);
		}
		else
			for (char c: string("REVERT"))
				expect(it, line.end(), c);

		expectations.emplace_back(SemanticsTestFunctionCall{
			std::move(signature),
			std::move(arguments),
			std::move(argumentBytes),
			std::move(ether),
			std::move(expectedResult),
			std::move(expectedBytes),
			std::move(expectedFormat)
		});
	}
	return expectations;
}

string SemanticsTest::ipcPath;
