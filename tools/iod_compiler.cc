#include <iostream>
#include <fstream>
#include <string>
#include <boost/regex.hpp>

// Iod to C++14 compiler.
//
//    Simple but useful syntaxic sugar on top of C++14.
//
//    Access to a symbol:                  @symbol      translated to    s::_Symbol
//    Access to a member via a symbol:     o@symbol     translated to    s::_Symbol.member_access(o);
//    Call to a method via a symbol:       o@symbol(42) translated to    s::_Symbol.method_call(o, 42);
//
//    All the symbols used in a C++ file are declared at the beginning of the generated file.
//
int main(int argc, char* argv[])
{
  using namespace std;

  if (argc == 0)
  {
    cout << "Usage: " << argv[0] << " input_cpp_file" << endl;
    return 1;
  }

  ifstream f(argv[1]);

  set<string> symbols;

  // Grammar:

  //  prefix identifier @ symbol_identifier (
  
  boost::regex symbol_regex("([^:A-Za-z0-9])([[:alnum:]]*)([[:blank:]]*)@[[:blank:]]*([[:alnum:]]+)[[:blank:]]*(([(][[:blank:]]*[)]?)?)");

  // 1: prefix
  // 2: variable name
  // 3: spaces
  // 4: symbol
  // 5: parenthesis
  
  vector<string> lines;

  auto to_safe_alias = [] (string s) {
    return string("_") + std::string(1, toupper(s[0])) + s.substr(1, s.size() - 1);
  };
  
  std::string line;
  bool in_raw_string = false;
  while (!f.eof())
  {
    getline(f, line);

    line = " " + line;

    std::vector<int> dbl_quotes_pos;
    bool escaped = false;
    for (int i = 0; i < line.size(); i++)
    {
      if (line[i] == '"' and !escaped) dbl_quotes_pos.push_back(i);
      else if (line[i] == '\\') escaped = !escaped;
      else escaped = false;
    }

    auto is_in_string = [&] (int p) {
      int i = 0;
      while (i < dbl_quotes_pos.size() and dbl_quotes_pos[i] <= p) i++;
      return i % 2;
    };
    
    auto fmt = [&] (const boost::smatch& s) -> std::string {
      if (is_in_string(s.position())) return s[0];
      else
      {
        std::string prefix, symbol, variable_name, parenthesis;
        prefix = string(s[1]);
        variable_name = s[2];
        string spaces = s[3];
        symbol = s[4]; 
        parenthesis = s[5];

        symbols.insert(symbol);
        ostringstream ss; ss << prefix;

        if (variable_name.length() > 0)
        {
          ss << "s::" << to_safe_alias(symbol);
          if (parenthesis.length())
          {
            ss << ".method_call(" << variable_name;
            if (parenthesis.back() == ')') ss << ")";
            else ss << ", ";
            // Add a comma only if the arg list is not empty.
            // bool empty_args = true;
            // int i = s[5].position();
            // while (i < line.size() and std::isspace(line[i])) i++;
            // if (i < line.size() and line[i] != ')') ss << ",";
          }
          else
            ss << ".member_access(" << variable_name << ")";
        }
        else ss << spaces << "s::" << to_safe_alias(symbol) << parenthesis;

        return ss.str();
      }
    };
    
    line = boost::regex_replace(line, symbol_regex, fmt);
    lines.push_back(line.substr(1, line.size() - 1));
  }

  auto& os = cout;
  os << "// Generated by the iod compiler."  << endl;
  os << "#include <iod/symbol.hh>" << endl;
  for (string s : symbols)
  {
    std::string safe_alias = to_safe_alias(s);
    os << "#ifndef IOD_SYMBOL_" << safe_alias << endl
       << "  iod_define_symbol(" <<  s << ", " << safe_alias << ")" << endl
       << "#endif" << endl;
  }

  for (auto line : lines)
    os << line << endl;
}