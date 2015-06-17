// Copyright (c) 2010-2011 SameGoal LLC.
// All Rights Reserved.
// Author: Andy Hochhaus <ahochhaus@samegoal.com>

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "code_generator.h"

#include <string>
#include <iostream>  // NOLINT
#include <sstream>  // NOLINT

#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/wire_format.h>
#include <google/protobuf/wire_format_lite_inl.h>

namespace google { namespace protobuf { namespace compiler { namespace js {
using internal::WireFormat;
using internal::WireFormatLite;

namespace {

string ToCamelCase(const string& input, bool lower_first) {
  bool capitalize_next = !lower_first;
  string result;
  result.reserve(input.size());

  for (int i = 0; i < input.size(); i++) {
    if (input[i] == '_') {
      capitalize_next = true;
    } else if (capitalize_next) {
      // Note:  I distrust ctype.h due to locales.
      if ('a' <= input[i] && input[i] <= 'z') {
        result.push_back(input[i] - 'a' + 'A');
      } else {
        result.push_back(input[i]);
      }
      capitalize_next = false;
    } else {
      result.push_back(input[i]);
    }
  }

  // Lower-case the first letter.
  if (lower_first && !result.empty() && 'A' <= result[0] && result[0] <= 'Z') {
      result[0] = result[0] - 'A' + 'a';
  }

  return result;
}

int sizeOfVarInt(long long v) {
  int n = 0;
  do {
    v >>= 7;
    n++;
  } while (v > 0);
  return n;
}

int CEscapeInternal(const char* src, int src_len, char* dest,
                    int dest_len, bool use_hex, bool utf8_safe) {
  const char* src_end = src + src_len;
  int used = 0;
  bool last_hex_escape = false;

  for (; src < src_end; src++) {
    if (dest_len - used < 2) {
       return -1;
    }

    bool is_hex_escape = false;
    switch (*src) {
      case '\n': dest[used++] = '\\'; dest[used++] = 'n';  break;
      case '\r': dest[used++] = '\\'; dest[used++] = 'r';  break;
      case '\t': dest[used++] = '\\'; dest[used++] = 't';  break;
      case '\"': dest[used++] = '\\'; dest[used++] = '\"'; break;
      case '\'': dest[used++] = '\\'; dest[used++] = '\''; break;
      case '\\': dest[used++] = '\\'; dest[used++] = '\\'; break;
      default:
        if ((!utf8_safe || static_cast<uint8>(*src) < 0x80) &&
            (!isprint(*src) ||
             (last_hex_escape && isxdigit(*src)))) {
          if (dest_len - used < 4) {
            return -1;
          }
          sprintf(dest + used, (use_hex ? "\\x%02x" : "\\%03o"),
                  static_cast<uint8>(*src));
          is_hex_escape = use_hex;
          used += 4;
        } else {
          dest[used++] = *src; break;
        }
    }
    last_hex_escape = is_hex_escape;
  }

  if (dest_len - used < 1){
    return -1;
  }

  dest[used] = '\0';
  return used;
}

int CEscapeString(const char* src, int src_len, char* dest, int dest_len) {
  return CEscapeInternal(src, src_len, dest, dest_len, false, false);
}

string CEscape(const string& src) {
  const int dest_length = src.size() * 4 + 1;
  scoped_array<char> dest(new char[dest_length]);
  const int len = CEscapeInternal(src.data(), src.size(),
                                  dest.get(), dest_length, false, false);
  return string(dest.get(), len);
}

string AndroidType(const google::protobuf::FieldDescriptor* field, bool fully_qualified) {
  string type;
  switch (field->type()) {
  case google::protobuf::FieldDescriptor::TYPE_BOOL:
    type = "Boolean"; 
    break;
  case google::protobuf::FieldDescriptor::TYPE_INT32:
    type = "Integer"; 
    break;
  case google::protobuf::FieldDescriptor::TYPE_INT64:
    type = "Long"; 
    break;
  case google::protobuf::FieldDescriptor::TYPE_FLOAT:
    type = "Float"; 
    break;
  case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
    type = "Double"; 
    break;
  case google::protobuf::FieldDescriptor::TYPE_STRING:
    type = "String"; 
    break;
  case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
    type = fully_qualified ? field->message_type()->full_name() : field->message_type()->name();
    break;
  case google::protobuf::FieldDescriptor::TYPE_ENUM:
    type = fully_qualified ? field->enum_type()->full_name() : field->enum_type()->name();
    break;
  default:
    type = "/* unknown */";
    break;
  }
  return type;
}

string AndroidTypeForField(const google::protobuf::FieldDescriptor* field, bool fully_qualified) {
  string type;
  type = AndroidType(field, fully_qualified);
  if (field->is_repeated()) {
    type = "List<" + type +">";
  }
  return type;
}

string DefaultValueForField(const google::protobuf::FieldDescriptor* field) {
  string default_value = "/* default value unknown */";
  if (field->is_repeated() || field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE || field->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
    default_value = "null";
  } else {
    switch (field->cpp_type()) {
      case internal::WireFormatLite::CPPTYPE_BOOL:
        default_value = field->is_optional() ? "null" : "false";
        if (field->has_default_value()) { 
          default_value = field->default_value_bool() ? "true" : "false";
        }
        break;
      case internal::WireFormatLite::CPPTYPE_INT32:
        default_value = field->is_optional() ? "null" : "0";
        if (field->has_default_value()) { 
          default_value = to_string(field->default_value_int32());
        }
        break;
      case internal::WireFormatLite::CPPTYPE_INT64:
        default_value = field->is_optional() ? "null" : "0";
        if (field->has_default_value()) { 
          default_value = to_string(field->default_value_int32());
        }
        break;
      case internal::WireFormatLite::CPPTYPE_FLOAT:
        default_value = field->is_optional() ? "null" : "0";
        if (field->has_default_value()) { 
          default_value = to_string(field->default_value_float());
        }
        break;
      case internal::WireFormatLite::CPPTYPE_DOUBLE:
        default_value = field->is_optional() ? "null" : "0";
        if (field->has_default_value()) { 
          default_value = to_string(field->default_value_double());
        }
        break;
      case internal::WireFormatLite::CPPTYPE_STRING:
        default_value = field->is_optional() ? "null" : "\"\"";
        if (field->has_default_value()) { 
          default_value = "\"" + CEscape(field->default_value_string()) + "\"";
        }
        break;
      default: 
        break;
    }
  }
  return default_value;
}

} // namespace

CodeGenerator::CodeGenerator(const std::string &name)
    : name_(name) {}

CodeGenerator::~CodeGenerator() {}

bool CodeGenerator::Generate(
    const google::protobuf::FileDescriptor *file,
    const std::string &/* parameter */,
    google::protobuf::compiler::OutputDirectory *output_directory,
    std::string *error) const {

  const std::string file_name = file->name();
  std::string output_file_name = file->name();
  std::size_t loc = output_file_name.rfind(".");
  output_file_name.erase(loc, output_file_name.length() - loc);
  std::string class_name = output_file_name;
  output_file_name.append(".pb.java");

  google::protobuf::internal::scoped_ptr<
      google::protobuf::io::ZeroCopyOutputStream> output(
          output_directory->Open(output_file_name));
  google::protobuf::io::Printer printer(output.get(), '$');
  printer.Print(
      "// Generated by the protocol buffer compiler.  DO NOT EDIT!\n");
  printer.Print("// source: $file_name$\n", "file_name", file_name);
  printer.Print("\n");
  printer.Print(
      "/**\n"
      " * @fileoverview Generated Protocol Buffer code for file $file_name$.\n"
      " */\n", "file_name", file_name);

  printer.Print(
      "\n"
      "import java.util.HashMap;\n"
      "import java.util.List;\n"
      "import java.util.Map;\n"
      "\n");

  printer.Print("public class $name$ {\n",
                 "name", class_name);
  printer.Indent();

  for (int i = 0; i < file->message_type_count(); ++i) {
    CodeGenerator::GenDescriptor(
        file->message_type(i),
        &printer);
  }

  printer.Print("static int sizeOfVarInt(long v) {\n");
  printer.Indent();
  printer.Print("int n = 0;\n");
  printer.Print("long x = v;\n");
  printer.Print("do {\n");
  printer.Indent();
  printer.Print("x = x >> 7;\n");
  printer.Print("n++;\n");
  printer.Outdent();
  printer.Print("} while (x > 0);\n");
  printer.Print("return n;\n");
  printer.Outdent();
  printer.Print("}\n");
  printer.Print("\n");

  printer.Print("static int sizeOfString(String s) {\n");
  printer.Indent();
  printer.Print("int length = s.length();\n");
  printer.Print("return sizeOfVarInt(length) + length;\n");
  printer.Outdent();
  printer.Print("}\n");

  printer.Outdent();
  printer.Print("}\n");

  for (int i = 0; i < file->enum_type_count(); ++i) {
    CodeGenerator::GenEnum(
        file->enum_type(i),
        &printer);
  }

  if (printer.failed()) {
    *error = "CodeGenerator detected write error.";
    return false;
  }

  return true;
}

void CodeGenerator::GenDescriptor(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  printer->Print("public static class $name$ {\n",
                 "name", message->name());
  printer->Indent();
  printer->Print("public final int sizeInBytes;\n");
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("public final $type$ $name$;\n",
                   "name", field->camelcase_name(),
                   "type", AndroidTypeForField(field, false));
  }
  printer->Print("\n");

  printer->Print("public $name$(int sizeInBytes",
                 "name", message->name());
  for (int i = 0, lastI = message->field_count() - 1; i <= lastI; ++i) {
    printer->Print(", ");
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("$type$ $name$",
                   "name", field->camelcase_name(),
                   "type", AndroidTypeForField(field, false));
  }
  printer->Print(") {\n");
  printer->Indent();
  printer->Print("this.sizeInBytes = sizeInBytes;\n");
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("this.$name$ = $name$;\n",
                   "name", field->camelcase_name());
  }
  printer->Outdent();
  printer->Print("}\n\n");

  CodeGenerator::GenMessage_toWriter(message, printer);
  printer->Print("\n");

  CodeGenerator::GenMessage_fromReader(message, printer);
  printer->Print("\n");

  CodeGenerator::GenMessage_sizeOf(message, printer);
  printer->Print("\n");

  CodeGenerator::GenMessage_equality(message, printer);
  printer->Print("\n");

  CodeGenerator::GenMessage_builder(message, printer);
  printer->Print("\n");

  for (int i = 0; i < message->nested_type_count(); ++i) {
    const google::protobuf::Descriptor *nested_type = message->nested_type(i);
    CodeGenerator::GenDescriptor(
        nested_type,
        printer);
    printer->Print("\n");
  }

  for (int i = 0; i < message->enum_type_count(); ++i) {
    const google::protobuf::EnumDescriptor *enum_desc = message->enum_type(i);
    CodeGenerator::GenEnum(
        enum_desc,
        printer);
    printer->Print("\n");
  }

  printer->Outdent();
  printer->Print("}\n\n");

  CodeGenerator::GenMessageBuilder(message, printer);

  printer->Print("\n");
}

void CodeGenerator::GenMessage_fromReader(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  printer->Print("public static $name$ fromReader(Reader r) {\n",
                 "name", message->name());
  printer->Indent();

  printer->Print("Map<String, Reader.TagMapValue> tagMap = new HashMap<String, Reader.TagMapValue>();\n");
    
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    string name = field->camelcase_name();
    string tag = to_string(WireFormatLite::MakeTag(field->number(), WireFormat::WireTypeForField(field)));
    string isRepeated = "false";
    
    if (field->is_repeated()) {
      isRepeated = "true";
    }

    printer->Print("tagMap.put(\"$name$\", new Reader.TagMapValue($tag$, $repeated$));",
                    "name", name,
                    "tag", tag,
                    "repeated", isRepeated);

    printer->Print("\n");
  }

  printer->Print("\n");
  printer->Print("r.pushTagMap(tagMap);\n\n");

  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    string default_value = DefaultValueForField(field);

    printer->Print("$type$ $name$ = $default_value$;\n",
                   "type", AndroidTypeForField(field, false),
                   "name", field->camelcase_name(),
                   "default_value", default_value);
  }
  printer->Print("\n");
  printer->Print("boolean continueLoop = true;\n");
  printer->Print("while (continueLoop) {\n");
  printer->Indent();
  printer->Print("switch (r.readTag()) {\n");
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    string tag = to_string(WireFormatLite::MakeTag(field->number(), WireFormat::WireTypeForField(field)));

    printer->Print("case $tag$: {\n",
                   "tag", tag);
    printer->Indent();

    string name = field->camelcase_name();

    if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
      string type = field->message_type()->full_name();

      printer->Print("long limit = r.pushLimit(r.readVarInt());\n");

      if (field->is_repeated()) {
        printer->Print("$name$.add($type$.fromReader(r));\n",
                       "type", type,
                       "name", name);
      } else {
        printer->Print("$name$ = $type$.fromReader(r);\n",
                       "type", type,
                       "name", name);
      }

      printer->Print("r.popLimit(limit);\n");
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
      string type = field->enum_type()->name();
      if (field->is_repeated()) {
        printer->Print("$name$.add($type$(rawValue: r.readVarInt())!)\n",
                       "type", type,
                       "name", name);
      } else {
        printer->Print("$name$ = $type$(rawValue: r.readVarInt())!\n",
                       "type", type,
                       "name", name);
      }
    } else {
      std::string read_func;
      std::string cast_func = "";
      if (field->type() == google::protobuf::FieldDescriptor::TYPE_BOOL) {
        read_func = "readBool";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT32) {
        cast_func = "(int)";
        read_func = "readVarInt";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT64) {
        read_func = "readVarInt";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_FLOAT) {
        read_func = "readFloat32";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_DOUBLE) {
        read_func = "readFloat64";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
        read_func = "readString";
      } else {
        read_func = "undefined";
      }

      if (field->is_repeated()) {
        printer->Print("$name$.add(r.$read_func$());\n",
                       "read_func",read_func,
                       "name", name);
      } else {
        printer->Print("$name$ = $cast_func$r.$read_func$();\n",
                       "cast_func", cast_func,
                       "read_func",read_func,
                       "name", name);
      }
    }
    printer->Print("break;\n");
    printer->Outdent();
    printer->Print("}\n");
  }
  printer->Print("default:\n");
  printer->Indent();
  printer->Print("continueLoop = false;\n");
  printer->Print("break;\n");
  printer->Outdent();
  //
  printer->Print("}\n");
  printer->Outdent();
  printer->Print("}\n\n");

  printer->Print("r.popTagMap();\n");

  printer->Print("\n");
  printer->Print("int sizeInBytes = $name$.sizeOf(",
                  "name", message->name());
  for (int i = 0, lastI = message->field_count() - 1; i <= lastI; ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("$name$",
                   "name", field->camelcase_name());
    if (i != lastI) {
      printer->Print(", ");
    }
  }
  printer->Print(");\n");

  printer->Print("return new $name$(sizeInBytes",
                  "name", message->name());
  for (int i = 0, lastI = message->field_count() - 1; i <= lastI; ++i) {
    printer->Print(", ");
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("$name$",
                   "name", field->camelcase_name());
  }
  printer->Print(");\n");

  printer->Outdent();
  printer->Print("}\n");
}

void CodeGenerator::GenMessage_builder(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  printer->Print("public static $name$Builder builder() {\n",
                 "name", message->name());
  printer->Indent();
  printer->Print("return new $name$Builder();\n",
                 "name", message->name());
  printer->Outdent();
  printer->Print("}\n");
}

void CodeGenerator::GenMessageBuilder(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  std::string builder_name = message->name() + "Builder";
  printer->Print("public static class $builder$ {\n",
                 "builder", builder_name);
  printer->Indent();
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    string default_value = DefaultValueForField(field);

    printer->Print("private $type$ $name$ = $default_value$;\n",
                   "name", field->camelcase_name(),
                   "type", AndroidTypeForField(field, true),
                   "default_value", default_value);
  }
  printer->Print("\n");

  printer->Print("public $builder$ clear() {\n",
                 "builder", builder_name);
  printer->Indent();

  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    string default_value = DefaultValueForField(field);

    printer->Print("this.$name$ = $default_value$;\n",
                   "name", field->camelcase_name(),
                   "default_value", default_value);
  }
  printer->Print("return this;\n");

  printer->Outdent();
  printer->Print("}\n\n");

  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    std::string field_name = field->camelcase_name();

    printer->Print("public $builder$ set$name$($type$ v) {\n",
                   "builder", builder_name,
                   "name", ToCamelCase(field->name(), false),
                   "type", AndroidTypeForField(field, true));
    printer->Indent();
    printer->Print("this.$name$ = v;\n",
                   "name", field_name);
    printer->Print("return this;\n");
    printer->Outdent();
    printer->Print("}\n\n");


    printer->Print("public $builder$ clear$name$() {\n",
                   "builder", builder_name,
                   "name", ToCamelCase(field->name(), false));
    printer->Indent();
    printer->Print("this.$name$ = $default_value$;\n",
                   "name", field_name,
                   "default_value", DefaultValueForField(field));
    printer->Print("return this;\n");
    printer->Outdent();
    printer->Print("}\n\n");
  }

  printer->Print("public $name$ build() {\n",
                 "name", message->name());
  printer->Indent();

  printer->Print("int sizeInBytes = $name$.sizeOf(",
                  "name", message->name());
  for (int i = 0, lastI = message->field_count() - 1; i <= lastI; ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("$name$",
                 "name", field->camelcase_name());
    if (i != lastI) {
      printer->Print(", ");
    }
  }
  printer->Print(");\n");
  printer->Print("return new $name$(sizeInBytes",
                  "name", message->name());
  for (int i = 0, lastI = message->field_count() - 1; i <= lastI; ++i) {
    printer->Print(", ");
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("$name$",
                   "name", field->camelcase_name());
  }
  printer->Print(");\n");

  printer->Outdent();
  printer->Print("}\n");

  printer->Outdent();
  printer->Print("}\n");
}

void CodeGenerator::GenMessage_toWriter(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  printer->Print("public void toWriter(Writer w) {\n");
  printer->Indent();


  printer->Print("Map<Integer, Writer.TagMapValue> tagMap = new HashMap<Integer, Writer.TagMapValue>(); \n");
    
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);

    string name = field->camelcase_name();
    string tag = to_string(WireFormatLite::MakeTag(field->number(), WireFormat::WireTypeForField(field)));
    string isRepeated = "false";

    if (field->is_repeated()) {
      isRepeated = "true";
    }

    printer->Print("tagMap.put($tag$, new Writer.TagMapValue(\"$name$\", $repeated$));",
                    "tag", tag,
                    "name", name,
                    "repeated", isRepeated);

    printer->Print("\n");
  }

  printer->Print("\n");
  printer->Print("w.pushTagMap(tagMap);\n\n");

  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    string name = "this." + field->camelcase_name();
    string tag = to_string(WireFormatLite::MakeTag(field->number(), WireFormat::WireTypeForField(field)));

    if (field->is_optional()) {
      printer->Print("if (null != $name$) {\n",
                     "name", name);
      printer->Indent();
    } else if (field->is_repeated()) {
      printer->Print("for ($type$ v : $name$) {\n",
                   "type", AndroidType(field, false),
                   "name", name);
      printer->Indent();
      name = "v";
    }
    
    if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
      printer->Print("w.writeTag($tag$);\n"
                     "w.writeVarInt($name$.sizeInBytes);\n"
                     "$name$.toWriter(w);\n",
                     "tag", tag,
                     "name", name);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
      printer->Print("w.writeTag($tag$);\n"
                     "w.writeVarInt($name$.rawValue);\n",
                     "tag", tag,
                     "name", name);
    } else {
      std::string write_func;
      if (field->type() == google::protobuf::FieldDescriptor::TYPE_BOOL) {
        write_func = "writeBool";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT32) {
        write_func = "writeVarInt";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT64) {
        write_func = "writeVarInt";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_FLOAT) {
        write_func = "writeFloat32";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_DOUBLE) {
        write_func = "writeFloat64";
      } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
        write_func = "writeString";
      } else {
        write_func = "undefined";
      }

      printer->Print("w.writeTag($tag$);\n"
                     "w.$write_func$($name$);\n",
                     "write_func", write_func,
                     "tag", tag,
                     "name", name);
    }

    if (field->is_repeated() || field->is_optional()) {
      printer->Outdent();
      printer->Print("}\n");
    }

    if (i < message->field_count() - 1) {
      printer->Print("\n");
    }
  }

  printer->Print("\n");
  printer->Print("w.popTagMap();\n");

  printer->Outdent();
  printer->Print("}\n");
}

void CodeGenerator::GenMessage_equality(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  string name = message->name();
  printer->Print("public boolean equals(Object object) {\n");
  printer->Indent();

  printer->Print("if (object instanceof $name$) {\n",
                 "name", name);
  printer->Indent();

  printer->Print("$name$ castObject = ($name$) object;\n",
                 "name", name);
  printer->Print("return (\n");
  printer->Indent();
  printer->Indent();
  printer->Print(" this.sizeInBytes == castObject.sizeInBytes\n");
  printer->Outdent();
  
  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("&& this.$name$ == castObject.$name$\n",
      "name", field->camelcase_name());
  }

  printer->Outdent();
  printer->Print(");\n");
  printer->Outdent();
  printer->Print("} else {\n");
  printer->Indent();
  printer->Print(" return false;\n");
  printer->Outdent();
  printer->Print("}\n");
  printer->Outdent();
  printer->Print("}\n");
}

void CodeGenerator::GenMessage_sizeOf(
    const google::protobuf::Descriptor *message,
    google::protobuf::io::Printer *printer) 
{
  printer->Print("static int sizeOf(");
  for (int i = 0, lastI = message->field_count() - 1; i <= lastI; ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    printer->Print("$type$ $name$",
                   "name", field->camelcase_name(),
                   "type", AndroidTypeForField(field, false));
    if (i != lastI) {
      printer->Print(", ");
    }
  }
  printer->Print(") {\n");
  printer->Indent();
  
  printer->Print("int n = 0;\n\n");

  for (int i = 0; i < message->field_count(); ++i) {
    const google::protobuf::FieldDescriptor *field = message->field(i);
    string name = field->camelcase_name();

    if (field->is_optional()) {
      printer->Print("if (null != $name$) {\n",
                     "name", name);
      printer->Indent();
    } else if (field->is_repeated()) {
      printer->Print("for ($type$ v : $name$) {\n",
                     "type", AndroidType(field, false),
                     "name", name);
      printer->Indent();
      name = "v";
    }

    int tag = WireFormatLite::MakeTag(field->number(), WireFormat::WireTypeForField(field));
    string size_of_tag = std::to_string(sizeOfVarInt(tag));

    if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
      printer->Print("n += $size_of_tag$ + sizeOfVarInt($name$.sizeInBytes) + $name$.sizeInBytes;\n",
                     "name", name, 
                     "size_of_tag", size_of_tag);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_ENUM) {
      printer->Print("n += $size_of_tag$ + sizeOfVarInt($name$.rawValue);\n",
                     "name", name, 
                     "size_of_tag", size_of_tag);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_BOOL) {
      printer->Print("n += $size_of_tag$ + 1;\n",
                     "size_of_tag", size_of_tag);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT32) {
      printer->Print("n += $size_of_tag$ + sizeOfVarInt($name$);\n",
                     "size_of_tag", size_of_tag,
                     "name", name);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_INT64) {
      printer->Print("n += $size_of_tag$ + sizeOfVarInt($name$);\n",
                     "size_of_tag", size_of_tag,
                     "name", name);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_FLOAT) {
      printer->Print("n += $size_of_tag$ + 4;\n",
                     "size_of_tag", size_of_tag);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_DOUBLE) {
      printer->Print("n += $size_of_tag$ + 8;\n",
                     "size_of_tag", size_of_tag);
    } else if (field->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
      printer->Print("n += $size_of_tag$ + sizeOfString($name$);\n",
                     "size_of_tag", size_of_tag,
                     "name", name);
    } else {
      // TODO: Other types
    }

    if (field->is_optional() || field->is_repeated()) {
      printer->Outdent();
      printer->Print("}\n");
    }
  }

  printer->Print("\n"
                 "return n;\n");

  printer->Outdent();
  printer->Print("}\n");
}

void CodeGenerator::GenEnum(
    const google::protobuf::EnumDescriptor *enum_desc,
    google::protobuf::io::Printer *printer) 
{
  printer->Print("public enum $name$: Int {\n",
                 "name", enum_desc->name());
  printer->Indent();
  for (int i = 0; i < enum_desc->value_count(); ++i) {
    string number = to_string(enum_desc->value(i)->number());
    printer->Print("case $key$ = $value$\n",
                   "key", ToCamelCase(enum_desc->value(i)->name(), false),
                   "value", number);
  }
  printer->Outdent();
  printer->Print("}\n\n");
}

}  // namespace js
}  // namespace compiler
}  // namespace protobuf
}  // namespace google