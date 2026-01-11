#include "Demangler.h"

#include <cctype>
#include <sstream>

// Credit to Arookas - https://github.com/intns/mapdas/blob/main/Helpers/Demangler.cs

namespace demangler
{

// ============================================================
// StringStream
// ============================================================

Demangler::StringStream::StringStream(const std::string& data) : Data(data), Position(0)
{
}

int Demangler::StringStream::Length() const
{
  return static_cast<int>(Data.size());
}

char Demangler::StringStream::Read()
{
  if (Position >= Length())
    return '\0';
  return Data[Position++];
}

char Demangler::StringStream::Peek() const
{
  if (Position >= Length())
    return '\0';
  return Data[Position];
}

char Demangler::StringStream::operator[](int index) const
{
  if (index >= Length())
    return '\0';
  return Data[index];
}

// ============================================================
// ComponentInfo
// ============================================================

Demangler::ComponentInfo::ComponentInfo(ComponentType t) : type(t), length(0)
{
}

Demangler::ComponentInfo::ComponentInfo(int array_len)
    : type(ComponentType::Array), length(array_len)
{
}

Demangler::ComponentInfo::ComponentInfo(const std::string& type_name)
    : type(ComponentType::Type), length(0), name(type_name)
{
}

Demangler::ComponentInfo::ComponentInfo(const std::string& params, const std::string& ret)
    : type(ComponentType::Func), length(0), name(ret), prms(params)
{
}

Demangler::ComponentInfo::ComponentInfo(ComponentType t, int len, const std::string& n,
                                        const std::string& p)
    : type(t), length(len), name(n), prms(p)
{
}

// ============================================================
// Demangling logic
// ============================================================

void Demangler::DemangleTemplate(StringStream& input, std::string& output)
{
  output.push_back('<');
  bool end = false;

  do
  {
    DemangleType(input, output);

    switch (input.Read())
    {
    case '>':
      end = true;
      break;
    case ',':
      output += ", ";
      break;
    case '\0':
      end = true;
      break;
    }
  } while (!end);

  output.push_back('>');
}

void Demangler::DemangleComponents(const std::vector<ComponentInfo>& components, size_t start,
                                   std::string& output)
{
  if (components.empty())
    return;

  ComponentType last = components[start].type;

  while (start < components.size())
  {
    const ComponentInfo& c = components[start];

    if (c.type != last)
    {
      output.push_back(' ');
      last = c.type;
    }

    switch (c.type)
    {
    case ComponentType::Const:
      output += "const";
      break;
    case ComponentType::Pointer:
      output.push_back('*');
      break;
    case ComponentType::Reference:
      output.push_back('&');
      break;
    case ComponentType::Unsigned:
      output += "unsigned";
      break;
    case ComponentType::Ellipsis:
      output += "...";
      break;
    case ComponentType::Void:
      output += "void";
      break;
    case ComponentType::Bool:
      output += "bool";
      break;
    case ComponentType::Char:
      output += "char";
      break;
    case ComponentType::WChar:
      output += "wchar_t";
      break;
    case ComponentType::Short:
      output += "short";
      break;
    case ComponentType::Int:
      output += "int";
      break;
    case ComponentType::Long:
      output += "long";
      break;
    case ComponentType::LongLong:
      output += "long long";
      break;
    case ComponentType::Float:
      output += "float";
      break;
    case ComponentType::Double:
      output += "double";
      break;
    case ComponentType::Type:
      output += c.name;
      break;

    case ComponentType::Func:
      output += c.name;
      output.push_back(' ');
      output.push_back('(');
      DemangleComponents(components, start + 1, output);
      output += ")(" + c.prms + ")";
      return;

    case ComponentType::Array:
    {
      size_t count = 0;
      while ((start + count) < components.size() &&
             components[start + count].type == ComponentType::Array)
        ++count;

      if (count > 0 && (start + count) < components.size())
      {
        output.push_back('(');
        DemangleComponents(components, start + count, output);
        output += ") ";
      }

      while (count-- > 0)
      {
        output += "[" + std::to_string(components[start + count].length) + "]";
      }
      return;
    }
    }

    ++start;
  }
}

void Demangler::DemangleType(StringStream& input, std::string& output)
{
  char c = input.Peek();

  if (c == '-' || std::isdigit(c))
  {
    bool literal = false;
    bool negative = false;

    if (c == '-')
    {
      input.Read();
      negative = true;
      literal = true;
    }

    int length = 0;
    while (std::isdigit(input.Peek()))
      length = length * 10 + (input.Read() - '0');

    if (input.Peek() == ',' || input.Peek() == '>')
      literal = true;

    if (literal)
    {
      if (negative)
        length = -length;
      output += std::to_string(length);
    }
    else
    {
      int start = input.Position;
      while ((input.Position - start) < length && input.Position < input.Length())
      {
        c = input.Read();
        if (c == '<')
          DemangleTemplate(input, output);
        else
          output.push_back(c);
      }
    }
    return;
  }

  bool end = false;
  std::vector<ComponentInfo> components;

  do
  {
    c = input.Read();
    if (c == '\0')
      end = true;

    switch (c)
    {
    case 'C':
      components.insert(components.begin(), ComponentInfo(ComponentType::Const));
      break;
    case 'P':
      components.insert(components.begin(), ComponentInfo(ComponentType::Pointer));
      break;
    case 'R':
      components.insert(components.begin(), ComponentInfo(ComponentType::Reference));
      break;
    case 'U':
      components.insert(components.begin(), ComponentInfo(ComponentType::Unsigned));
      break;

    case 'A':
    {
      int len = 0;
      while ((c = input.Read()) != '_')
        len = len * 10 + (c - '0');
      components.insert(components.begin(), ComponentInfo(len));
      break;
    }

    case 'v':
      components.insert(components.begin(), ComponentInfo(ComponentType::Void));
      end = true;
      break;
    case 'i':
      components.insert(components.begin(), ComponentInfo(ComponentType::Int));
      end = true;
      break;
    case 'f':
      components.insert(components.begin(), ComponentInfo(ComponentType::Float));
      end = true;
      break;
    case 'd':
      components.insert(components.begin(), ComponentInfo(ComponentType::Double));
      end = true;
      break;

    case 'Q':
    {
      int count = input.Read() - '0';
      std::string name;
      while (count-- > 0)
      {
        DemangleType(input, name);
        if (count > 0)
          name += "::";
      }
      components.insert(components.begin(), ComponentInfo(name));
      end = true;
      break;
    }

    case 'F':
    {
      std::string prms;
      while (input.Peek() != '_' && input.Peek() != '\0')
      {
        if (!prms.empty())
          prms += ", ";
        DemangleType(input, prms);
      }
      input.Read();
      std::string ret;
      DemangleType(input, ret);
      components.insert(components.begin(), ComponentInfo(prms == "void" ? "" : prms, ret));
      end = true;
      break;
    }

    default:
      if (std::isdigit(c))
      {
        input.Position--;
        std::string name;
        DemangleType(input, name);
        components.insert(components.begin(), ComponentInfo(name));
        end = true;
      }
      break;
    }
  } while (!end);

  DemangleComponents(components, 0, output);
}

int Demangler::ScanNameEnd(const StringStream& input)
{
  int end = input.Length();

  for (int i = input.Position; i < input.Length() - 1; ++i)
  {
    if (input[i] == '_' && input[i + 1] == '_')
      end = i;
  }

  return end;
}

std::string Demangler::DemangleName(StringStream& input)
{
  std::string output;
  int end = ScanNameEnd(input);

  while (input.Position < end)
  {
    char c = input.Read();
    if (c == '<')
      DemangleTemplate(input, output);
    else
      output.push_back(c);
  }

  if (end < input.Length())
    input.Position += 2;

  return output;
}

std::string Demangler::Demangle(const std::string& symbol)
{
  StringStream input(symbol);
  std::string output;

  std::string name = DemangleName(input);
  std::string type;
  std::string prms;
  bool constant = false;

  if (input.Position < input.Length() && input.Peek() != 'F')
  {
    DemangleType(input, output);
    type = output;
    output.clear();
  }

  if (input.Peek() == 'C')
  {
    input.Read();
    constant = true;
  }

  if (input.Peek() == 'F')
  {
    input.Read();
    while (input.Position < input.Length())
    {
      if (!output.empty())
        output += ", ";
      DemangleType(input, output);
    }
    prms = output;
    output.clear();
  }

  if (!type.empty())
    output += type + "::";

  output += name;

  if (!prms.empty())
    output += "(" + prms + ")";

  if (constant)
    output += " const";

  return output;
}

}  // namespace demangler
