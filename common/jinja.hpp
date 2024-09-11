/*
  The absolute minimum needed to run chat templates seen in the wild.

  Models have increasingly complex templates (e.g. Llama 3.1, Hermes 2 Pro w/ tool_use), so we need a proper template engine to get the best out of them.

  TODO:
  - Functionary 3.2:
    - selectattr("type", "defined")
      {{ users|selectattr("is_active") }}
      {{ users|selectattr("email", "none") }}
      {{ data | selectattr('name', '==', 'Jinja') | list | last }}
    - Macro nested set scope = global?
      {%- macro get_param_type(param) -%}
        {%- set param_type = "any" -%}

    - map(attribute="type")
  - Add |dict_update({...})
  - Add {%- if tool.parameters.properties | length == 0 %}
  - Add `{% raw %}{{ broken }{% endraw %}` https://jbmoelker.github.io/jinja-compat-tests/tags/raw/
  - Add more functions https://jinja.palletsprojects.com/en/3.0.x/templates/#builtin-filters
    - https://jbmoelker.github.io/jinja-compat-tests/
*/
#pragma once

#include "llama.h"
#include "common.h"

#include <iostream>
#include <string>
#include <vector>
#include <regex>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <json.hpp>

using json = nlohmann::ordered_json;

/* Backport make_unique from C++14. */
template <class T, class... Args>
typename std::unique_ptr<T> nonstd_make_unique(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

namespace jinja {

class Context;

/* Values that behave roughly like in Python.
 * jinja templates deal with objects by reference so we can't just json for arrays & objects,
 * but we do for primitives.
 */
class Value : public std::enable_shared_from_this<Value> {
public:
  struct CallableArgs {
    std::vector<Value> args;
    std::vector<std::pair<std::string, Value>> kwargs;

    bool empty() const {
      return args.empty() && kwargs.empty();
    }

    void expectArgs(const std::string & method_name, const std::pair<size_t, size_t> & pos_count, const std::pair<size_t, size_t> & kw_count) const {
      if (args.size() < pos_count.first || args.size() > pos_count.second || kwargs.size() < kw_count.first || kwargs.size() > kw_count.second) {
        std::ostringstream out;
        out << method_name << " must have between " << pos_count.first << " and " << pos_count.second << " positional arguments and between " << kw_count.first << " and " << kw_count.second << " keyword arguments";
        throw std::runtime_error(out.str());
      }
    }


    json dump() const {
      json res = json::array();
      for (const auto & arg : args) {
        res.push_back(arg.dump());
      }
      for (const auto & kwarg : kwargs) {
        res.push_back({kwarg.first, kwarg.second.dump()});
      }
      return res;
    }
  };
  
  using CallableType = std::function<Value(Context &, const CallableArgs &)>;
  using FilterType = std::function<Value(Context &, const CallableArgs &)>;

private:
  using ObjectType = std::unordered_map<json, Value>;  // Only contains primitive keys
  using ArrayType = std::vector<Value>;

  std::shared_ptr<ArrayType> array_;
  std::shared_ptr<ObjectType> object_;
  std::shared_ptr<CallableType> callable_;
  json primitive_;

  Value(const std::shared_ptr<ArrayType> & array) : array_(array) {}
  Value(const std::shared_ptr<ObjectType> & object) : object_(object) {}
  Value(const std::shared_ptr<CallableType> & callable) : object_(std::make_shared<ObjectType>()), callable_(callable) {}

  void dump(std::ostringstream & out, int indent = -1, int level = 0) const {
    auto print_indent = [&](int level) {
      if (indent > 0) for (int i = 0, n = level * indent; i < n; ++i) out << ' ';
    };
    auto print_sub_sep = [&]() {
      out << ',';
      if (indent < 0) out << ' ';
      else {
        out << '\n';
        print_indent(level + 1);
      }
    };

    if (is_null()) out << "null";
    else if (array_) {
      out << "[";
      if (indent >= 0) {
        out << "\n";
        print_indent(level + 1);
      }
      for (size_t i = 0; i < array_->size(); ++i) {
        if (i) print_sub_sep();
        (*array_)[i].dump(out, indent, level + 1);
      }
      if (indent >= 0) {
        out << "\n";
        print_indent(level);
      }
      out << "]";
    } else if (object_) {
      out << "{";
      if (indent >= 0) {
        out << "\n";
        print_indent(level + 1);
      }
      for (auto begin = object_->begin(), it = begin; it != object_->end(); ++it) {
        if (it != begin) print_sub_sep();
        auto key_dump = it->first.dump();
        if (it->first.is_string()) {
          out << key_dump;
        } else {
          out << '"' << key_dump << '"';
        }
        out << ": ";
        it->second.dump(out, indent, level + 1);
      }
      if (indent >= 0) {
        out << "\n";
        print_indent(level);
      }
      out << "}";
    } else if (callable_) {
      throw std::runtime_error("Cannot dump callable to JSON");
    } else if (is_boolean()) {
      out << (*this ? "True" : "False");
    } else {
      out << primitive_.dump();
    }
  }

public:
  Value() {}
  Value(const bool& v) : primitive_(v) {}
  Value(const int64_t & v) : primitive_(v) {}
  Value(const double& v) : primitive_(v) {}
  Value(const nullptr_t &) {}
  Value(const std::string & v) : primitive_(v) {}
  Value(const char * v) : primitive_(std::string(v)) {}

  Value(const json & v) {
    if (v.is_object()) {
      auto object = std::make_shared<ObjectType>();
      for (auto it = v.begin(); it != v.end(); ++it) {
        (*object)[it.key()] = it.value();
      }
      object_ = std::move(object);
    } else if (v.is_array()) {
      auto array = std::make_shared<ArrayType>();
      for (const auto& item : v) {
        array->push_back(Value(item));
      }
      array_ = array;
    } else {
      primitive_ = v;
    }
  }
  
  std::vector<Value> keys() const {
    if (!object_) throw std::runtime_error("Value is not an object: " + dump());

    std::vector<Value> res;
    for (const auto& item : *object_) {
      res.push_back(item.first);
    }
    return res;
  }

  size_t size() const {
    if (is_object()) return object_->size();
    if (is_array()) return array_->size();
    throw std::runtime_error("Value is not an array or object: " + dump());
  }

  static Value array(const std::vector<Value> values = {}) {
    auto array = std::make_shared<ArrayType>();
    for (const auto& item : values) {
      array->push_back(item);
    }
    return Value(array);
  }
  static Value object(const std::shared_ptr<ObjectType> object = std::make_shared<ObjectType>()) {
    return Value(object);
  }
  static Value callable(const CallableType & callable) {
    return Value(std::make_shared<CallableType>(callable));
  }
  static Value context(const Value & values = {});

  void insert(size_t index, const Value& v) {
    if (!array_)
      throw std::runtime_error("Value is not an array: " + dump());
    array_->insert(array_->begin() + index, v);
  }
  void push_back(const Value& v) {
    if (!array_)
      throw std::runtime_error("Value is not an array: " + dump());
    array_->push_back(v);
  }

  Value get(const Value& key) const {
    if (array_) {
      auto index = key.get<int>();
      return array_->at(index < 0 ? array_->size() + index : index);
    } else if (object_) {
      if (!key.is_hashable()) throw std::runtime_error("Unashable type: " + dump());
      auto it = object_->find(key.primitive_);
      if (it == object_->end()) return Value();
      return it->second;
    }
    throw std::runtime_error("Value is not an array or object: " + dump());
  }

  void set(const Value& key, const Value& value) {
    if (!is_object()) throw std::runtime_error("Value is not an object: " + dump());
    if (!key.is_hashable()) throw std::runtime_error("Unashable type: " + dump());
    (*object_)[key.primitive_] = value;
  }

  Value call(Context & context, const Value::CallableArgs & args) const {
    if (!callable_) throw std::runtime_error("Value is not callable: " + dump());
    return (*callable_)(context, args);
  }

  bool is_object() const { return !!object_; }
  bool is_array() const { return !!array_; }
  bool is_callable() const { return !!callable_; }
  bool is_null() const { return !object_ && !array_ && primitive_.is_null() && !callable_; }
  bool is_boolean() const { return primitive_.is_boolean(); }
  bool is_number_integer() const { return primitive_.is_number_integer(); }
  bool is_number_float() const { return primitive_.is_number_float(); }
  bool is_number() const { return primitive_.is_number(); }
  bool is_string() const { return primitive_.is_string(); }

  bool is_primitive() const { return !array_ && !object_ && !callable_; }
  bool is_hashable() const { return is_primitive(); }

  bool empty() const {
    if (is_null())
      throw std::runtime_error("Undefined value or reference");
    if (is_string()) return primitive_.empty();
    if (is_array()) return array_->empty();
    if (is_object()) return object_->empty();
    return false;
  }

  operator bool() const {
    if (is_null()) return false;
    if (is_boolean()) return get<bool>();
    if (is_number()) return get<double>() != 0;
    if (is_string()) return !get<std::string>().empty();
    if (is_array()) return !empty();
    return true;
  } 

  bool operator<(const Value & other) const {
    if (is_null())
      throw std::runtime_error("Undefined value or reference");
    if (is_number() && other.is_number()) return get<double>() < other.get<double>();
    if (is_string() && other.is_string()) return get<std::string>() < other.get<std::string>();
    throw std::runtime_error("Cannot compare values: " + dump() + " < " + other.dump());
  }

  bool operator==(const Value & other) const {
    if (callable_ || other.callable_) {
      if (callable_.get() != other.callable_.get()) return false;
    }
    if (array_) {
      if (!other.array_) return false;
      if (array_->size() != other.array_->size()) return false;
      for (size_t i = 0; i < array_->size(); ++i) {
        if (!(*array_)[i] || !(*other.array_)[i] || (*array_)[i] != (*other.array_)[i]) return false;
      }
      return true;
    } else if (object_) {
      if (!other.object_) return false;
      if (object_->size() != other.object_->size()) return false;
      for (const auto& item : *object_) {
        if (!item.second || !other.object_->count(item.first) || item.second != other.object_->at(item.first)) return false;
      }
      return true;
    } else {
      return primitive_ == other.primitive_;
    }
  }

  bool contains(const char * key) const { return contains(std::string(key)); }
  bool contains(const std::string & key) const {
    if (array_) {
      return false;
    } else if (object_) {
      return object_->find(key) != object_->end();
    } else {
      throw std::runtime_error("contains can only be called on arrays and objects: " + dump());
    }
  }
  bool contains(const Value & value) const {
    if (is_null())
      throw std::runtime_error("Undefined value or reference");
    if (array_) {
      for (const auto& item : *array_) {
        if (item && item == value) return true;
      }
      return false;
    } else if (object_) {
      if (!value.is_hashable()) throw std::runtime_error("Unashable type: " + value.dump());
      return object_->find(value.primitive_) != object_->end();
    } else {
      throw std::runtime_error("contains can only be called on arrays and objects: " + dump());
    }
  }
  void erase(const std::string & key) {
    if (object_) throw std::runtime_error("Value is not an object: " + dump());
    object_->erase(key);
  }
  const Value& at(size_t index) const {
    if (is_null())
      throw std::runtime_error("Undefined value or reference");
    if (is_array()) return array_->at(index);
    if (is_object()) return object_->at(index);
    throw std::runtime_error("Value is not an array or object: " + dump());
  }
  const Value& at(const Value & index) const {
    if (!index.is_hashable()) throw std::runtime_error("Unashable type: " + dump());
    if (is_array()) return array_->at(index.get<int>());
    if (is_object()) return object_->at(index.primitive_);
    return this->at(index);
  }

  template <typename T>
  T get(const std::string & key, T default_value) const {
    if (!contains(key)) return default_value;
    return at(key).get<T>();
  }
  
  template <typename T>
  T get() const {
    if (is_primitive()) return primitive_.get<T>();
    throw std::runtime_error("get<T> not defined for this value type: " + dump());
  }

  template <>
  std::vector<std::string> get<std::vector<std::string>>() const {
    if (array_) {
      std::vector<std::string> res;
      for (const auto& item : *array_) {
        res.push_back(item.get<std::string>());
      }
      return res;
    }
    throw std::runtime_error("get<string> not defined for this value type: " + dump());
  }
  std::string dump(int indent=-1) const {
    // Note: get<json>().dump(indent) would work but [1, 2] would be dumped to [1,2] instead of [1, 2]
    std::ostringstream out;
    dump(out, indent);
    return out.str();
  }

  template <>
  json get<json>() const {
    if (is_primitive()) return primitive_;
    if (is_null()) return json();
    if (array_) {
      std::vector<json> res;
      for (const auto& item : *array_) {
        res.push_back(item.get<json>());
      }
      return res;
    }
    if (object_) {
      json res = json::object();
      for (const auto& item : *object_) {
        const auto & key = item.first;
        auto json_value = item.second.get<json>();
        if (key.is_string()) {
          res[key.get<std::string>()] = json_value;
        } else if (key.is_primitive()) {
          res[key.dump()] = json_value;
        } else {
          throw std::runtime_error("Invalid key type for conversion to JSON: " + key.dump());
        }
      }
      if (is_callable()) {
        res["__callable__"] = true;
      }
      return res;
    }
    throw std::runtime_error("get<json> not defined for this value type: " + dump());
  }

  Value operator-() const {
      if (is_number_integer())
        return -get<int64_t>();
      else
        return -get<double>();
  }
  std::string to_str() const {
    if (is_string()) return get<std::string>();
    if (is_number_integer()) return std::to_string(get<int64_t>());
    if (is_number_float()) return std::to_string(get<double>());
    if (is_boolean()) return get<bool>() ? "True" : "False";
    if (is_null()) return "None";
    return dump();
  }
  Value operator+(const Value& rhs) const {
      if (is_string() || rhs.is_string())
        return to_str() + rhs.to_str();
      else if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() + rhs.get<int64_t>();
      else
        return get<double>() + rhs.get<double>();
  }
  Value operator-(const Value& rhs) const {
      if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() - rhs.get<int64_t>();
      else
        return get<double>() - rhs.get<double>();
  }
  Value operator*(const Value& rhs) const {
      if (is_string() && rhs.is_number_integer()) {
        std::ostringstream out;
        for (int i = 0, n = rhs.get<int64_t>(); i < n; ++i) {
          out << to_str();
        }
        return out.str();
      }
      else if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() * rhs.get<int64_t>();
      else
        return get<double>() * rhs.get<double>();
  }
  Value operator/(const Value& rhs) const {
      if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() / rhs.get<int64_t>();
      else
        return get<double>() / rhs.get<double>();
  }
  Value operator%(const Value& rhs) const {
    return get<int64_t>() % rhs.get<int64_t>();
  }
};

} // namespace jinja

namespace std {

// hash specialization for Value
template <>
struct hash<jinja::Value> {
  size_t operator()(const jinja::Value & v) const {
    if (!v.is_hashable())
      throw std::runtime_error("Unsupported type for hashing: " + v.dump());
    return std::hash<json>()(v.get<json>());
  }
};

} // namespace std

namespace jinja {

class Context : public std::enable_shared_from_this<Context> {
 protected:
  Value values_;
  std::shared_ptr<Context> parent_;
public:
  Context(Value && values = Value::object(), const std::shared_ptr<Context> & parent = nullptr) : values_(std::move(values)), parent_(parent) {
    if (!values_.is_object())
      throw std::runtime_error("Context values must be an object: " + values_.dump());
  }

  static std::shared_ptr<Context> builtins();
  static std::shared_ptr<Context> make(Value && values);

  json dump() const;

  std::vector<Value> keys() const {
    return values_.keys();
  }
  virtual Value get(const Value & key) const {
    if (values_.contains(key)) return values_.at(key);
    if (parent_) return parent_->get(key);
    return Value();
  }
  virtual const Value & at(const Value & key) const {
    if (values_.contains(key)) return values_.at(key);
    if (parent_) return parent_->at(key);
    throw std::runtime_error("Undefined variable: " + key.dump());
  }
  bool contains(const std::string & key) const {
    return contains(Value(key));
  }
  virtual bool contains(const Value & key) const {
    if (values_.contains(key)) return true;
    if (parent_) return parent_->contains(key);
    return false;
  }
  virtual void set(const Value & key, const Value & value) {
    values_.set(key, value);
  }
};

class MacroContext : public Context {
public:
  MacroContext(Value && arg_values = Value(), const std::shared_ptr<Context> & parent = nullptr) 
    : Context(std::move(arg_values), parent) {}
  void set(const Value & key, const Value & value) override {
    if (values_.contains(key)) {
      values_.set(key, value);
    } else if (parent_) {
      parent_->set(key, value);
    } else {
      throw std::runtime_error("Undefined variable: " + key.dump());
    }
  }
};

class Expression {
public:
    struct CallableArgs {
        std::vector<std::unique_ptr<Expression>> args;
        std::vector<std::pair<std::string, std::unique_ptr<Expression>>> kwargs;

        void expectArgs(const std::string & method_name, const std::pair<size_t, size_t> & pos_count, const std::pair<size_t, size_t> & kw_count) const {
          if (args.size() < pos_count.first || args.size() > pos_count.second || kwargs.size() < kw_count.first || kwargs.size() > kw_count.second) {
            std::ostringstream out;
            out << method_name << " must have between " << pos_count.first << " and " << pos_count.second << " positional arguments and between " << kw_count.first << " and " << kw_count.second << " keyword arguments";
            throw std::runtime_error(out.str());
          }
        }

        json dump() const {
          json res = json::array();
          for (const auto & arg : args) {
            res.push_back(arg->dump());
          }
          for (const auto & kwarg : kwargs) {
            res.push_back(json::object({{"name", kwarg.first}, {"value", kwarg.second->dump()}}));
          }
          return res;
        }

        Value::CallableArgs evaluate(Context & context) const {
            Value::CallableArgs vargs;
            for (const auto& arg : this->args) {
                vargs.args.push_back(arg->evaluate(context));
            }
            for (const auto& arg : this->kwargs) {
                vargs.kwargs.push_back({arg.first, arg.second->evaluate(context)});
            }
            return vargs;
        }
    };

    using CallableParams = std::vector<std::pair<std::string, std::unique_ptr<Expression>>>;

    virtual ~Expression() = default;
    virtual Value evaluate(Context & context) const = 0;
    virtual json dump() const = 0;
};

json dumpParams(const Expression::CallableParams & params) {
  json res = json::array();
  for (const auto & param : params) {
    res.push_back(json::object({{"name", param.first}, {"value", param.second->dump()}}));
  }
  return res;
}

static void destructuring_assign(const std::vector<std::string> & var_names, Context & context, const Value& item) {
  if (var_names.size() == 1) {
      context.set(var_names[0], item);
  } else {
      if (!item.is_array() || item.size() != var_names.size()) {
          throw std::runtime_error("Mismatched number of variables and items in destructuring assignment");
      }
      for (size_t i = 0; i < var_names.size(); ++i) {
          context.set(var_names[i], item.at(i));
      }
  }
}

enum SpaceHandling { Keep, Strip };

class TemplateToken {
public:
    enum class Type { Text, Expression, If, Else, Elif, EndIf, For, EndFor, Set, EndSet, NamespacedSet, Comment, Block, EndBlock, Macro, EndMacro };

    static std::string typeToString(Type t) {
        switch (t) {
            case Type::Text: return "text";
            case Type::Expression: return "expression";
            case Type::If: return "if";
            case Type::Else: return "else";
            case Type::Elif: return "elif";
            case Type::EndIf: return "endif";
            case Type::For: return "for";
            case Type::EndFor: return "endfor";
            case Type::Set: return "set";
            case Type::EndSet: return "endset";
            case Type::NamespacedSet: return "namespaced set";
            case Type::Comment: return "comment";
            case Type::Block: return "block";
            case Type::EndBlock: return "endblock";
            case Type::Macro: return "macro";
            case Type::EndMacro: return "endmacro";
        }
        return "Unknown";
    }

    TemplateToken(Type type, size_t pos, SpaceHandling pre, SpaceHandling post) : type(type), pos(pos), pre_space(pre), post_space(post) {}
    virtual ~TemplateToken() = default;

    Type type;
    size_t pos;
    SpaceHandling pre_space = SpaceHandling::Keep;
    SpaceHandling post_space = SpaceHandling::Keep;
};

struct TextTemplateToken : public TemplateToken {
    std::string text;
    TextTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& t) : TemplateToken(Type::Text, pos, pre, post), text(t) {}
};

struct ExpressionTemplateToken : public TemplateToken {
    std::unique_ptr<Expression> expr;
    ExpressionTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, std::unique_ptr<Expression> && e) : TemplateToken(Type::Expression, pos, pre, post), expr(std::move(e)) {}
};

struct IfTemplateToken : public TemplateToken {
    std::unique_ptr<Expression> condition;
    IfTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, std::unique_ptr<Expression> && c) : TemplateToken(Type::If, pos, pre, post), condition(std::move(c)) {}
};

struct ElifTemplateToken : public TemplateToken {
    std::unique_ptr<Expression> condition;
    ElifTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, std::unique_ptr<Expression> && c) : TemplateToken(Type::Elif, pos, pre, post), condition(std::move(c)) {}
};

struct ElseTemplateToken : public TemplateToken {
    ElseTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(Type::Else, pos, pre, post) {}
};

struct EndIfTemplateToken : public TemplateToken {
   EndIfTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(Type::EndIf, pos, pre, post) {}
};

struct BlockTemplateToken : public TemplateToken {
    std::string name;
    BlockTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& n) : TemplateToken(Type::Block, pos, pre, post), name(n) {}
};

struct EndBlockTemplateToken : public TemplateToken {
    EndBlockTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(Type::EndBlock, pos, pre, post) {}
};

struct MacroTemplateToken : public TemplateToken {
    std::string name;
    Expression::CallableParams params;
    MacroTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& n, Expression::CallableParams && p)
      : TemplateToken(Type::Macro, pos, pre, post), name(n), params(std::move(p)) {}
};

struct EndMacroTemplateToken : public TemplateToken {
    EndMacroTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(Type::EndMacro, pos, pre, post) {}
};

struct ForTemplateToken : public TemplateToken {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> iterable;
    std::unique_ptr<Expression> condition;
    bool recursive;
    ForTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::vector<std::string> & vns, std::unique_ptr<Expression> && iter,
      std::unique_ptr<Expression> && c, bool r)
      : TemplateToken(Type::For, pos, pre, post), var_names(vns), iterable(std::move(iter)), condition(std::move(c)), recursive(r) {}
};

struct EndForTemplateToken : public TemplateToken {
    EndForTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(Type::EndFor, pos, pre, post) {}
};

struct SetTemplateToken : public TemplateToken {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> value;
    SetTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::vector<std::string> & vns, std::unique_ptr<Expression> && v)
      : TemplateToken(Type::Set, pos, pre, post), var_names(vns), value(std::move(v)) {}
};

struct EndSetTemplateToken : public TemplateToken {
    EndSetTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(Type::EndSet, pos, pre, post) {}
};

struct NamespacedSetTemplateToken : public TemplateToken {
    std::string ns, name;
    std::unique_ptr<Expression> value;
    NamespacedSetTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& ns, const std::string& name, std::unique_ptr<Expression> && v)
      : TemplateToken(Type::NamespacedSet, pos, pre, post), ns(ns), name(name), value(std::move(v)) {}
};

struct CommentTemplateToken : public TemplateToken {
    std::string text;
    CommentTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& t) : TemplateToken(Type::Comment, pos, pre, post), text(t) {}
};

class TemplateNode {
public:
    virtual ~TemplateNode() = default;
    virtual void render(std::ostringstream& oss, Context & context) const = 0;
    std::string render(Context & context) const {
        std::ostringstream oss;
        render(oss, context);
        return oss.str();
    }
    virtual json dump() const = 0;
};

class SequenceNode : public TemplateNode {
    std::vector<std::unique_ptr<TemplateNode>> children;
public:
    SequenceNode(std::vector<std::unique_ptr<TemplateNode>> && c) : children(std::move(c)) {}
    void render(std::ostringstream& oss, Context & context) const override {
        for (const auto& child : children) child->render(oss, context);
    }
    json dump() const override {
        json res = json::array();
        for (const auto& child : children) {
            res.push_back(child->dump());
        }
        return {{"sequence", res}};
    }
};

class TextNode : public TemplateNode {
    std::string text;
public:
    TextNode(const std::string& t) : text(t) {}
    void render(std::ostringstream& oss, Context &) const override { oss << text; }
    json dump() const override { return {{"text", text}}; }
};

class ExpressionNode : public TemplateNode {
    std::unique_ptr<Expression> expr;
public:
    ExpressionNode(std::unique_ptr<Expression> && e) : expr(std::move(e)) {}
    void render(std::ostringstream& oss, Context & context) const override {
      auto result = expr->evaluate(context);
      if (result.is_string()) {
          oss << result.get<std::string>();
      } else if (result.is_boolean()) {
          oss << (result.get<bool>() ? "True" : "False");
      } else if (!result.is_null()) {
          oss << result.dump();
      }
  }
    json dump() const override { return {{"expression", expr->dump()}}; }
};

class IfNode : public TemplateNode {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> cascade;
public:
    IfNode(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> && c)
        : cascade(std::move(c)) {}
    void render(std::ostringstream& oss, Context & context) const override {
    for (const auto& branch : cascade) {
        auto enter_branch = true;
        if (branch.first) {
          enter_branch = !!branch.first->evaluate(context);
        }
        if (enter_branch) {
            branch.second->render(oss, context);
            return;
        }
    }
  }
    json dump() const override {
        json res = json::array();
        for (const auto& branch : cascade) {
            if (branch.first) {
                res.push_back({{"elif", {{"condition", branch.first->dump()}, {"branch", branch.second->dump()}}}});
            } else {
                res.push_back({{"else", branch.second->dump()}});
            }
        }
        return {{"if", res}};
    }
};

class ForNode : public TemplateNode {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> iterable;
    std::unique_ptr<Expression> condition;
    std::unique_ptr<TemplateNode> body;
    std::unique_ptr<TemplateNode> else_body;
    bool recursive;
public:
    ForNode(const std::vector<std::string> & vns, std::unique_ptr<Expression> && iter,
      std::unique_ptr<Expression> && c, std::unique_ptr<TemplateNode> && b, bool r, std::unique_ptr<TemplateNode> && eb)
            : var_names(vns), iterable(std::move(iter)), condition(std::move(c)), body(std::move(b)), recursive(r), else_body(std::move(eb)) {}
    void render(std::ostringstream& oss, Context & context) const override {
      // https://jinja.palletsprojects.com/en/3.0.x/templates/#for

      auto iterable_value = iterable->evaluate(context);
      if (!iterable_value.is_array()) {
        throw std::runtime_error("For loop iterable must be iterable");
      }

      Value::CallableType loop_function;

      std::function<void(const Value&)> visit = [&](const Value& iter) {
          auto filtered_items = Value::array();
          for (size_t i = 0, n = iter.size(); i < n; ++i) {
              auto item = iter.at(i);
              destructuring_assign(var_names, context, item);
              if (!condition || condition->evaluate(context)) {
                filtered_items.push_back(item);
              }
          }
          if (filtered_items.empty()) {
            if (else_body) {
              else_body->render(oss, context);
            }
          } else {
              auto loop = recursive ? Value::callable(loop_function) : Value::object();
              loop.set("length", (int64_t) filtered_items.size());

              size_t cycle_index = 0;
              loop.set("cycle", Value::callable([&](Context & context, const Value::CallableArgs & args) {
                  if (args.args.empty() || !args.kwargs.empty()) {
                      throw std::runtime_error("cycle() expects at least 1 positional argument and no named arg");
                  }
                  auto item = args.args[cycle_index];
                  cycle_index = (cycle_index + 1) % args.args.size();
                  return item;
              }));
              auto loop_context = nonstd_make_unique<Context>(Value::object(), context.shared_from_this());
              loop_context->set("loop", loop);
              for (size_t i = 0, n = filtered_items.size(); i < n; ++i) {
                  auto & item = filtered_items.at(i);
                  destructuring_assign(var_names, *loop_context, item);
                  loop.set("index", (int64_t) i + 1);
                  loop.set("index0", (int64_t) i);
                  loop.set("revindex", (int64_t) (n - i));
                  loop.set("revindex0", (int64_t) (n - i - 1));
                  loop.set("length", (int64_t) n);
                  loop.set("first", i == 0);
                  loop.set("last", i == n - 1);
                  loop.set("previtem", i > 0 ? filtered_items.at(i - 1) : Value());
                  loop.set("nextitem", i < n - 1 ? filtered_items.at(i + 1) : Value());
                  body->render(oss, *loop_context);
              }
          }
      };

      if (recursive) {
        loop_function = [&](Context & context, const Value::CallableArgs & args) {
            if (args.args.size() != 1 || !args.kwargs.empty() || !args.args[0].is_array()) {
                throw std::runtime_error("loop() expects exactly 1 positional iterable argument");
            }
            auto & items = args.args[0];
            visit(items);
            return Value();
        };
      }

      visit(iterable_value);
  }

    json dump() const override {
        json res = {{"var_names", var_names}, {"iterable", iterable->dump()}};
        if (condition) {
            res["if"] = condition->dump();
        }
        res["body"] = body->dump();
        if (else_body) {
            res["else"] = else_body->dump();
        }
        return {{"for", res}};
    }
};

class BlockNode : public TemplateNode {
    std::string name;
    std::unique_ptr<TemplateNode> body;
public:
    BlockNode(const std::string& n, std::unique_ptr<TemplateNode> && b)
        : name(n), body(std::move(b)) {}
    void render(std::ostringstream& oss, Context & context) const override {
        body->render(oss, context);
    }
    json dump() const override {
        return {{"block", {{"name", name}, {"body", body->dump()}}}};
    }
};

class MacroNode : public TemplateNode {
    std::string name;
    Expression::CallableParams params;
    std::unique_ptr<TemplateNode> body;
    std::unordered_map<std::string, size_t> named_param_positions;
public:
    MacroNode(const std::string& n, Expression::CallableParams && p, std::unique_ptr<TemplateNode> && b)
        : name(n), params(std::move(p)), body(std::move(b)) {
        for (size_t i = 0; i < params.size(); ++i) {
          const auto & name = params[i].first;
          if (!name.empty()) {
            named_param_positions[name] = i;
          }
        }
    }
    void render(std::ostringstream& oss, Context & macro_context) const override {
        macro_context.set(name, Value::callable([&](Context & context, const Value::CallableArgs & args) {
            auto call_context = macro_context;
            std::vector<bool> param_set(params.size(), false);
            for (size_t i = 0, n = args.args.size(); i < n; i++) {
                auto & arg = args.args[i];
                if (i >= params.size()) throw std::runtime_error("Too many positional arguments for macro " + name);
                param_set[i] = true;
                auto & param_name = params[i].first;
                call_context.set(param_name, arg);
            }
            for (size_t i = 0, n = args.kwargs.size(); i < n; i++) {
                auto & arg = args.kwargs[i];
                auto & arg_name = arg.first;
                auto it = named_param_positions.find(arg_name);
                if (it == named_param_positions.end()) throw std::runtime_error("Unknown parameter name for macro " + name + ": " + arg_name);
                
                call_context.set(arg_name, arg.second);
                param_set[it->second] = true;
            }
            // Set default values for parameters that were not passed
            for (size_t i = 0, n = params.size(); i < n; i++) {
                if (!param_set[i] && params[i].second != nullptr) {
                    call_context.set(params[i].first, params[i].second->evaluate(context));
                }
            }
            return body->render(call_context);
        }));
    }
    json dump() const override {
        return {{"macro", {{"name", name}, {"params", dumpParams(params)}, {"body", body->dump()}}}};
    }
};

class SetNode : public TemplateNode {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> value;
    std::unique_ptr<TemplateNode> template_value;
public:
    SetNode(const std::vector<std::string> & vns, std::unique_ptr<Expression> && v, std::unique_ptr<TemplateNode> && tv)
        : var_names(vns), value(std::move(v)), template_value(std::move(tv)) {
          if (value && template_value) {
            throw std::runtime_error("Cannot have both value and template value in set node");
          }
          if (template_value && var_names.size() != 1) {
            throw std::runtime_error("Destructuring assignment is only supported with a single variable name");
          }
        }
    void render(std::ostringstream &, Context & context) const override {
      if (template_value) {
        auto value = template_value->render(context);
        context.set(var_names[0], value);
      } else {
        destructuring_assign(var_names, context, value->evaluate(context));
      }
    }
    json dump() const override {
        json res = {{"var_names", var_names}, {"value", value ? value->dump() : "null"}, {"template_value", template_value ? template_value->dump() : "null"}};
        return {{"set", res}};
    }
};

class NamespacedSetNode : public TemplateNode {
    std::string ns, name;
    std::unique_ptr<Expression> value;
public:
    NamespacedSetNode(const std::string& ns, const std::string& name, std::unique_ptr<Expression> && v)
      : ns(ns), name(name), value(std::move(v)) {}
    void render(std::ostringstream &, Context & context) const override {
        auto ns_value = context.get(ns);
        if (!ns_value.is_object()) throw std::runtime_error("Namespace '" + ns + "' is not an object");
        ns_value.set(name, this->value->evaluate(context));
    }
    json dump() const override {
        return {{"set", {{"namespaces", ns}, {"name", name}, {"value", value->dump()}}}};
    }
};

class IfExpr : public Expression {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> then_expr;
    std::unique_ptr<Expression> else_expr;
public:
    IfExpr(std::unique_ptr<Expression> && c, std::unique_ptr<Expression> && t, std::unique_ptr<Expression> && e)
        : condition(std::move(c)), then_expr(std::move(t)), else_expr(std::move(e)) {}
    Value evaluate(Context & context) const override {
        return condition->evaluate(context) ? then_expr->evaluate(context) : else_expr->evaluate(context);
    }
    json dump() const override {
        return {{"if", condition->dump()}, {"then", then_expr->dump()}, {"else", else_expr->dump()}};
    }
};

class LiteralExpr : public Expression {
    Value value;
public:
    LiteralExpr(const Value& v) : value(v) {}
    Value evaluate(Context &) const override { return value; }
    json dump() const override { return value.dump(); }
};

class ArrayExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> elements;
public:
    ArrayExpr(std::vector<std::unique_ptr<Expression>> && e) : elements(std::move(e)) {}
    Value evaluate(Context & context) const override {
        auto result = Value::array();
        for (const auto& e : elements) {
            result.push_back(e->evaluate(context));
        }
        return result;
    }
    json dump() const override {
        json result = json::array();
        for (const auto& e : elements) {
            result.push_back(e->dump());
        }
        return result;
    }
};

class DictExpr : public Expression {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> elements;
public:
    DictExpr(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> && e) : elements(std::move(e)) {}
    Value evaluate(Context & context) const override {
        auto result = Value::object();
        for (const auto& e : elements) {
            result.set(e.first->evaluate(context), e.second->evaluate(context));
        }
        return result;
    }
    json dump() const override {
        json kvs = json::array();
        for (const auto& e : elements) {
            kvs.push_back({
              {"key", e.first->dump()},
              {"value", e.second->dump()},
            });
        }
        return {{"dict", kvs}};
    }
};

class VariableExpr : public Expression {
    std::string name;
public:
    VariableExpr(const std::string& n) : name(n) {}
    std::string get_name() const { return name; }
    Value evaluate(Context & context) const override {
        if (!context.contains(name)) {
            std::cerr << "Failed to find '" << name << "' in context (has keys: " << Value::array(context.keys()).dump() << ")" << std::endl;
            return Value();
        }
        return context.at(name);
    }
    json dump() const override { return {{"variable", name}}; }
};

class SliceExpr : public Expression {
public:
    std::unique_ptr<Expression> start, end;
    SliceExpr(std::unique_ptr<Expression> && s, std::unique_ptr<Expression> && e) : start(std::move(s)), end(std::move(e)) {}
    Value evaluate(Context &) const override {
        throw std::runtime_error("SliceExpr not implemented");
    }
    json dump() const override {
        return {{"slice", {start->dump(), end->dump()}}};
    }
};

class SubscriptExpr : public Expression {
    std::unique_ptr<Expression> base;
    std::unique_ptr<Expression> index;
public:
    SubscriptExpr(std::unique_ptr<Expression> && b, std::unique_ptr<Expression> && i)
        : base(std::move(b)), index(std::move(i)) {}
    Value evaluate(Context & context) const override {
        auto target_value = base->evaluate(context);
        if (auto slice = dynamic_cast<SliceExpr*>(index.get())) {
          if (!target_value.is_array()) throw std::runtime_error("Subscripting non-array");

          auto start = slice->start ? slice->start->evaluate(context).get<size_t>() : 0;
          auto end = slice->end ? slice->end->evaluate(context).get<size_t>() : target_value.size();
          auto result = Value::array();
          for (auto i = start; i < end; ++i) {
            result.push_back(target_value.at(i));
          }
          return result;
        } else {
          auto index_value = index->evaluate(context);
          if (target_value.is_null()) {
            if (auto t = dynamic_cast<VariableExpr*>(base.get())) {
              throw std::runtime_error("'" + t->get_name() + "' is " + (context.contains(t->get_name()) ? "null" : "not defined"));
            }
            throw std::runtime_error("Trying to access property '" +  index_value.dump() + "' on null!");
          }
          return target_value.get(index_value);
        }
    }
    json dump() const override {
        return {{"subscript", {{"base", base->dump()}, {"index", index->dump()}}}};
    }
};

class UnaryOpExpr : public Expression {
public:
    enum class Op { Plus, Minus, LogicalNot };
private:
    std::unique_ptr<Expression> expr;
    Op op;
public:
    UnaryOpExpr(std::unique_ptr<Expression> && e, Op o) : expr(std::move(e)), op(o) {}
    Value evaluate(Context & context) const override {
        auto e = expr->evaluate(context);
        switch (op) {
            case Op::Plus: return e;
            case Op::Minus: return -e;
            case Op::LogicalNot: return !e;
        }
        throw std::runtime_error("Unknown unary operator");
    }
    json dump() const override {
        return {{"unary", {{"op", (int) op}, {"expr", expr->dump()}}}};
    }
};

class BinaryOpExpr : public Expression {
public:
    enum class Op { StrConcat, Add, Sub, Mul, MulMul, Div, DivDiv, Mod, Eq, Ne, Lt, Gt, Le, Ge, And, Or, In, NotIn, Is, IsNot };
private:
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    Op op;
public:
    BinaryOpExpr(std::unique_ptr<Expression> && l, std::unique_ptr<Expression> && r, Op o)
        : left(std::move(l)), right(std::move(r)), op(o) {}
    Value evaluate(Context & context) const override {
        auto l = left->evaluate(context);
        
        auto do_eval = [&](const Value & l) -> Value {
          if (op == Op::Is || op == Op::IsNot) {
            auto t = dynamic_cast<VariableExpr*>(right.get());
            if (!t) throw std::runtime_error("Right side of 'is' operator must be a variable");

            auto eval = [&]() {
              const auto & name = t->get_name();
              if (name == "none") return l.is_null();
              if (name == "boolean") return l.is_boolean();
              if (name == "integer") return l.is_number_integer();
              if (name == "float") return l.is_number_float();
              if (name == "number") return l.is_number();
              if (name == "string") return l.is_string();
              if (name == "mapping") return l.is_object();
              if (name == "iterable") return l.is_array();
              if (name == "sequence") return l.is_array();
              if (name == "defined") return !l.is_null();
              throw std::runtime_error("Unknown type for 'is' operator: " + name);
            };
            auto value = eval();
            return Value(op == Op::Is ? value : !value);
          }

          if (op == Op::And) {
            if (!l) return Value(false);
            return !!right->evaluate(context);
          } else if (op == Op::Or) {
            if (l) return Value(true);
            return !!right->evaluate(context);
          }

          auto r = right->evaluate(context);
          switch (op) {
              case Op::StrConcat: return l.to_str() + r.to_str();
              case Op::Add:       return l + r;
              case Op::Sub:       return l - r;
              case Op::Mul:       return l * r;
              case Op::Div:       return l / r;
              case Op::MulMul:    return std::pow(l.get<double>(), r.get<double>());
              case Op::DivDiv:    return l.get<int64_t>() / r.get<int64_t>();
              case Op::Mod:       return l.get<int64_t>() % r.get<int64_t>();
              case Op::Eq:        return l == r;
              case Op::Ne:        return l != r;
              case Op::Lt:        return l < r;
              case Op::Gt:        return l > r;
              case Op::Le:        return l <= r;
              case Op::Ge:        return l >= r;
              case Op::In:        return r.is_array() && r.contains(l);
              case Op::NotIn:     return !(r.is_array() && r.contains(l));
              default:            break;
          }
          throw std::runtime_error("Unknown binary operator");
        };

        if (l.is_callable()) {
          return Value::callable([l, do_eval](Context & context, const Value::CallableArgs & args) {
            auto ll = l.call(context, args);
            return do_eval(ll); //args[0].second);
          });
        } else {
          return do_eval(l);
        }
    }
    json dump() const override {
        return {{"binary", {{"op", (int) op}, {"left", left->dump()}, {"right", right->dump()}}}};
    }
};

class MethodCallExpr : public Expression {
    std::unique_ptr<Expression> object;
    std::string method;
    Expression::CallableArgs args;
    // std::vector<std::pair<std::string, std::unique_ptr<Expression>>> args;
public:
    MethodCallExpr(std::unique_ptr<Expression> && obj, const std::string& m, Expression::CallableArgs && a)
        : object(std::move(obj)), method(m), args(std::move(a)) {}
    const std::string& get_method() const { return method; }
    Value evaluate(Context & context) const override {
        auto obj = object->evaluate(context);
        if (obj.is_array()) {
          if (method == "append") {
              args.expectArgs("append method", {1, 1}, {0, 0});
              obj.push_back(args.args[0]->evaluate(context));
              return Value();
          } else if (method == "insert") {
              args.expectArgs("insert method", {2, 2}, {0, 0});
              auto index = args.args[0]->evaluate(context).get<int>();
              if (index < 0 || index > obj.size()) throw std::runtime_error("Index out of range for insert method");
              obj.insert(index, args.args[1]->evaluate(context));
              return Value();
          }
        } else if (obj.is_object()) {
          if (method == "items") {
            args.expectArgs("items method", {0, 0}, {0, 0});
            auto result = Value::array();
            for (const auto& key : obj.keys()) {
              result.push_back(Value::array({key, obj.at(key)}));
            }
            return result;
          } else if (method == "get") {
            args.expectArgs("get method", {1, 2}, {0, 0});
            auto key = args.args[0]->evaluate(context);
            if (args.args.size() == 1) {
              return obj.contains(key) ? obj.at(key) : Value();
            } else {
              return obj.contains(key) ? obj.at(key) : args.args[1]->evaluate(context);
            }
          } else if (obj.contains(method)) {
            auto callable = obj.at(method);
            if (!callable.is_callable()) {
              throw std::runtime_error("Property '" + method + "' is not callable");
            }
            Value::CallableArgs vargs = args.evaluate(context);
            return callable.call(context, vargs);
          }
        }
        throw std::runtime_error("Unknown method: " + method);
    }
    json dump() const override {
        return {{"call", {{"method", method}, {"object", object->dump()}, "args", args.dump()}}};
    }
};

class CallExpr : public Expression {
public:
    std::unique_ptr<Expression> object;
    Expression::CallableArgs args;
    CallExpr(std::unique_ptr<Expression> && obj, Expression::CallableArgs && a)
        : object(std::move(obj)), args(std::move(a)) {}
    Value evaluate(Context & context) const override {
        auto obj = object->evaluate(context);
        if (!obj.is_callable()) {
          throw std::runtime_error("Object is not callable: " + obj.dump(2));
        }
        return obj.call(context, args.evaluate(context));
    }
    json dump() const override {
        return {{"call", {{"object", object->dump()}, {"args", args.dump()}}}};
    }
};

class FilterExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> parts;
public:
    FilterExpr(std::vector<std::unique_ptr<Expression>> && p) : parts(std::move(p)) {}
    Value evaluate(Context & context) const override {
        Value result;
        bool first = true;
        for (const auto& part : parts) {
          if (first) {
            first = false;
            result = part->evaluate(context);
          } else {
            if (auto ce = dynamic_cast<CallExpr*>(part.get())) {
              auto target = ce->object->evaluate(context);
              result = target.call(context, ce->args.evaluate(context));
            } else {
              auto callable = part->evaluate(context);
              Value::CallableArgs args;
              args.args.push_back(result);
              result = callable.call(context, args);
            }
          }
        }
        return result;
    }

    void prepend(std::unique_ptr<Expression> && e) {
        parts.insert(parts.begin(), std::move(e));
    }
    json dump() const override {
        json result = {{"filter", json::array()}};
        for (const auto& part : parts) {
            result["filter"].push_back(part->dump());
        }
        return result;
    }
};

static std::string strip(const std::string & s) {
  static std::regex trailing_spaces_regex("^\\s+|\\s+$");
  return std::regex_replace(s, trailing_spaces_regex, "");
}

static std::string html_escape(const std::string & s) {
  std::string result;
  result.reserve(s.size());
  for (const auto & c : s) {
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += c; break;
    }
  }
  return result;
}

class Parser {
private:
    using CharIterator = std::string::const_iterator;

    std::string template_str;
    CharIterator start, end, it;
      
    Parser(const std::string& template_str) : template_str(template_str) {
      start = it = this->template_str.begin();
      end = this->template_str.end();
    }

    bool consumeSpaces(SpaceHandling space_handling = SpaceHandling::Strip) {
      if (space_handling == SpaceHandling::Strip) {
        while (it != end && std::isspace(*it)) ++it;
      }
      return true;
    }

    std::unique_ptr<std::string> parseString() {
      auto doParse = [&](char quote) -> std::unique_ptr<std::string> {
        if (it == end || *it != quote) return nullptr;
        std::string result;
        bool escape = false;
        for (++it; it != end; ++it) {
          if (escape) {
            escape = false;
            switch (*it) {
              case 'n': result += '\n'; break;
              case 'r': result += '\r'; break;
              case 't': result += '\t'; break;
              case 'b': result += '\b'; break;
              case 'f': result += '\f'; break;
              case '\\': result += '\\'; break;
              default: 
                if (*it == quote) {
                  result += quote;
                } else {
                  result += *it;
                }
                break;
            }
          } else if (*it == '\\') {
            escape = true;
          } else if (*it == quote) {
              ++it;
            return nonstd_make_unique<std::string>(result);
          } else {
            result += *it;
          }
        }
        return nullptr;
      };

      consumeSpaces();
      if (it == end) return nullptr;
      if (*it == '"') return doParse('"');
      if (*it == '\'') return doParse('\'');
      return nullptr;
    }

    json parseNumber(CharIterator& it, const CharIterator& end) {
        auto before = it;
        consumeSpaces();
        auto start = it;
        bool hasDecimal = false;
        bool hasExponent = false;

        if (it != end && (*it == '-' || *it == '+')) ++it;

        while (it != end) {
          if (std::isdigit(*it)) {
            ++it;
          } else if (*it == '.') {
            if (hasDecimal) throw std::runtime_error("Multiple decimal points");
            hasDecimal = true;
            ++it;
          } else if (it != start && (*it == 'e' || *it == 'E')) {
            if (hasExponent) throw std::runtime_error("Multiple exponents");
            hasExponent = true;
            ++it;
          } else {
            break;
          }
        }
        if (start == it) {
          it = before;
          return json(); // No valid characters found
        }

        std::string str(start, it);
        try {
          return json::parse(str);
        } catch (json::parse_error& e) {
          throw std::runtime_error("Failed to parse number: '" + str + "' (" + std::string(e.what()) + ")");
          return json();
        }
    }

    /** integer, float, bool, string */
    std::unique_ptr<Value> parseConstant() {
      auto start = it;
      consumeSpaces();
      if (it == end) return nullptr;
      if (*it == '"' || *it == '\'') {
        auto str = parseString();
        if (str) return nonstd_make_unique<Value>(*str);
      }
      static std::regex prim_tok(R"(true\b|True\b|false\b|False\b|None\b)");
      auto token = consumeToken(prim_tok);
      if (!token.empty()) {
        if (token == "true" || token == "True") return nonstd_make_unique<Value>(true);
        if (token == "false" || token == "False") return nonstd_make_unique<Value>(false);
        if (token == "None") return nonstd_make_unique<Value>(nullptr);
        throw std::runtime_error("Unknown constant token: " + token);
      }

      auto number = parseNumber(it, end);
      if (!number.is_null()) return nonstd_make_unique<Value>(number);

      it = start;
      return nullptr;
    }

    class expression_parsing_error : public std::runtime_error {
      const CharIterator it;
     public:
      expression_parsing_error(const std::string & message, const CharIterator & it)
        : std::runtime_error(message), it(it) {}
      size_t get_pos(const CharIterator & begin) const {
        return std::distance(begin, it);
      }
    };

    bool peekSymbols(const std::vector<std::string> & symbols) const {
        for (const auto & symbol : symbols) {
            if (std::distance(it, end) >= symbol.size() && std::string(it, it + symbol.size()) == symbol) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> consumeTokenGroups(const std::regex & regex, SpaceHandling space_handling = SpaceHandling::Strip) {
        auto start = it;
        consumeSpaces(space_handling);
        std::smatch match;
        if (std::regex_search(it, end, match, regex) && match.position() == 0) {
            it += match[0].length();
            std::vector<std::string> ret;
            for (size_t i = 0, n = match.size(); i < n; ++i) {
                ret.push_back(match[i].str());
            }
            return ret;
        }
        it = start;
        return {};
    }
    std::string consumeToken(const std::regex & regex, SpaceHandling space_handling = SpaceHandling::Strip) {
        auto start = it;
        consumeSpaces(space_handling);
        std::smatch match;
        if (std::regex_search(it, end, match, regex) && match.position() == 0) {
            it += match[0].length();
            return match[0].str();
        }
        it = start;
        return "";
    }

    std::string consumeToken(const std::string & token, SpaceHandling space_handling = SpaceHandling::Strip) {
        auto start = it;
        consumeSpaces(space_handling);
        if (std::distance(it, end) >= token.size() && std::string(it, it + token.size()) == token) {
            it += token.size();
            return token;
        }
        it = start;
        return "";
    }

    /**
      * - Expression = LogicalOr ("if" IfExpression)?
      * - IfExpression = LogicalOr "else" Expression
      * - LogicalOr = LogicalAnd ("or" LogicalOr)? = LogicalAnd ("or" LogicalAnd)*
      * - LogicalAnd = LogicalCompare ("and" LogicalAnd)? = LogicalCompare ("and" LogicalCompare)*
      * - LogicalCompare = StringConcat ((("==" | "!=" | "<" | ">" | "<=" | ">=" | "in") StringConcat) | "is" identifier)?
      * - StringConcat = MathPow ("~" LogicalAnd)?
      * - MathPow = MathPlusMinus ("**" MathPow)? = MatPlusMinus ("**" MathPlusMinus)*
      * - MathPlusMinus = MathMulDiv (("+" | "-") MathPlusMinus)? = MathMulDiv (("+" | "-") MathMulDiv)*
      * - MathMulDiv = MathUnaryPlusMinus (("*" | "/" | "//" | "%") MathMulDiv)? = MathUnaryPlusMinus (("*" | "/" | "//" | "%") MathUnaryPlusMinus)*
      * - MathUnaryPlusMinus = ("+" | "-" | "!")? ValueExpression ("|" FilterExpression)?
      * - FilterExpression = identifier CallParams ("|" FilterExpression)? = identifier CallParams ("|" identifier CallParams)*
      * - ValueExpression = (identifier | number | string | bool | BracedExpressionOrArray | Tuple | Dictionary ) SubScript? CallParams?
      * - BracedExpressionOrArray = "(" Expression ("," Expression)* ")"
      * - Tuple = "[" (Expression ("," Expression)*)? "]"
      * - Dictionary = "{" (string "=" Expression ("," string "=" Expression)*)? "}"
      * - SubScript = ("[" Expression "]" | "." identifier CallParams? )+
      * - CallParams = "(" ((identifier "=")? Expression ("," (identifier "=")? Expression)*)? ")"
      */
    std::unique_ptr<Expression> parseExpression(bool allow_if_expr = true) {
        auto left = parseLogicalOr();
        if (it == end) return left;

        if (!allow_if_expr) return left;

        static std::regex if_tok(R"(if\b)");
        if (consumeToken(if_tok).empty()) {
          return left;
        }

        auto if_expr = parseIfExpression();
        return nonstd_make_unique<IfExpr>(std::move(left), std::move(if_expr.first), std::move(if_expr.second));
    }

    std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>> parseIfExpression() {
        auto condition = parseLogicalOr();
        if (!condition) throw std::runtime_error("Expected condition expression");

        static std::regex else_tok(R"(else\b)");
        if (consumeToken(else_tok).empty()) throw std::runtime_error("Expected 'else' keyword");
        
        auto else_expr = parseExpression();
        if (!else_expr) throw std::runtime_error("Expected 'else' expression");

        return std::make_pair(std::move(condition), std::move(else_expr));
    }

    std::unique_ptr<Expression> parseLogicalOr() {
        auto left = parseLogicalAnd();
        if (!left) throw std::runtime_error("Expected left side of 'logical or' expression");

        static std::regex or_tok(R"(or\b)");
        while (!consumeToken(or_tok).empty()) {
            auto right = parseLogicalAnd();
            if (!right) throw std::runtime_error("Expected right side of 'or' expression");
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::Or);
        }
        return left;
    }

    std::unique_ptr<Expression> parseLogicalAnd() {
        auto left = parseLogicalCompare();
        if (!left) throw std::runtime_error("Expected left side of 'logical and' expression");

        static std::regex and_tok(R"(and\b)");
        while (!consumeToken(and_tok).empty()) {
            auto right = parseLogicalCompare();
            if (!right) throw std::runtime_error("Expected right side of 'and' expression");
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::And);
        }
        return left;
    }

    std::unique_ptr<Expression> parseLogicalCompare() {
        auto left = parseStringConcat();
        if (!left) throw std::runtime_error("Expected left side of 'logical compare' expression");

        static std::regex compare_tok(R"(==|!=|<=?|>=?|in\b|is\b|not[\n\s]+in\b)");
        static std::regex not_tok(R"(not\b)");
        std::string op_str;
        while (!(op_str = consumeToken(compare_tok)).empty()) {
            if (op_str == "is") {
              auto negated = !consumeToken(not_tok).empty();

              auto identifier = parseIdentifier();
              if (identifier.empty()) throw std::runtime_error("Expected identifier after 'is' keyword");

              return nonstd_make_unique<BinaryOpExpr>(
                  std::move(left), nonstd_make_unique<VariableExpr>(identifier),
                  negated ? BinaryOpExpr::Op::IsNot : BinaryOpExpr::Op::Is);
            }
            auto right = parseStringConcat();
            if (!right) throw std::runtime_error("Expected right side of 'logical compare' expression");
            BinaryOpExpr::Op op;
            if (op_str == "==") op = BinaryOpExpr::Op::Eq;
            else if (op_str == "!=") op = BinaryOpExpr::Op::Ne;
            else if (op_str == "<") op = BinaryOpExpr::Op::Lt;
            else if (op_str == ">") op = BinaryOpExpr::Op::Gt;
            else if (op_str == "<=") op = BinaryOpExpr::Op::Le;
            else if (op_str == ">=") op = BinaryOpExpr::Op::Ge;
            else if (op_str == "in") op = BinaryOpExpr::Op::In;
            // if op_str starts with "not" it must be "not in"
            else if (op_str.substr(0, 3) == "not") op = BinaryOpExpr::Op::NotIn;
            else throw std::runtime_error("Unknown comparison operator: " + op_str);
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }
        return left;
    }

    Expression::CallableParams parseParameters() {
        consumeSpaces();
        if (consumeToken("(").empty()) throw std::runtime_error("Expected opening parenthesis in param list");

        Expression::CallableParams result;
        
        while (it != end) {
            if (!consumeToken(")").empty()) {
                return result;
            }
            auto expr = parseExpression();
            if (!expr) throw std::runtime_error("Expected expression in call args");

            if (auto ident = dynamic_cast<VariableExpr*>(expr.get())) {
                if (!consumeToken("=").empty()) {
                    auto value = parseExpression();
                    if (!value) throw std::runtime_error("Expected expression in for named arg");
                    result.emplace_back(ident->get_name(), std::move(value));
                } else {
                    result.emplace_back(ident->get_name(), nullptr);
                }
            } else {
                result.emplace_back(std::string(), std::move(expr));
            }
            if (consumeToken(",").empty()) {
              if (consumeToken(")").empty()) {
                throw std::runtime_error("Expected closing parenthesis in call args");
              }
              return result;
            }
        }
        throw std::runtime_error("Expected closing parenthesis in call args");
    }

    Expression::CallableArgs parseCallArgs() {
        consumeSpaces();
        if (consumeToken("(").empty()) throw std::runtime_error("Expected opening parenthesis in call args");

        Expression::CallableArgs result;
        
        while (it != end) {
            if (!consumeToken(")").empty()) {
                return result;
            }
            auto expr = parseExpression();
            if (!expr) throw std::runtime_error("Expected expression in call args");

            if (auto ident = dynamic_cast<VariableExpr*>(expr.get())) {
                if (!consumeToken("=").empty()) {
                    auto value = parseExpression();
                    if (!value) throw std::runtime_error("Expected expression in for named arg");
                    result.kwargs.emplace_back(ident->get_name(), std::move(value));
                } else {
                    result.args.emplace_back(std::move(expr));
                }
            } else {
                result.args.emplace_back(std::move(expr));
            }
            if (consumeToken(",").empty()) {
              if (consumeToken(")").empty()) {
                throw std::runtime_error("Expected closing parenthesis in call args");
              }
              return result;
            }
        }
        throw std::runtime_error("Expected closing parenthesis in call args");
    }

    std::string parseIdentifier() {
        static std::regex ident_regex(R"((?!not|is|and|or|del)[a-zA-Z_]\w*)");
        return consumeToken(ident_regex);
    }

    std::unique_ptr<Expression> parseStringConcat() {
        auto left = parseMathPow();
        if (!left) throw std::runtime_error("Expected left side of 'string concat' expression");

        static std::regex concat_tok(R"(~(?!\}))");
        if (!consumeToken(concat_tok).empty()) {
            auto right = parseLogicalAnd();
            if (!right) throw std::runtime_error("Expected right side of 'string concat' expression");
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::StrConcat);
        }
        return left;
    }

    std::unique_ptr<Expression> parseMathPow() {
        auto left = parseMathPlusMinus();
        if (!left) throw std::runtime_error("Expected left side of 'math pow' expression");

        while (!consumeToken("**").empty()) {
            auto right = parseMathPlusMinus();
            if (!right) throw std::runtime_error("Expected right side of 'math pow' expression");
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::MulMul);
        }
        return left;
    }

    std::unique_ptr<Expression> parseMathPlusMinus() {
        static std::regex plus_minus_tok(R"(\+|-(?![}%#]\}))");

        auto left = parseMathMulDiv();
        if (!left) throw std::runtime_error("Expected left side of 'math plus/minus' expression");
        std::string op_str;
        while (!(op_str = consumeToken(plus_minus_tok)).empty()) {
            auto right = parseMathMulDiv();
            if (!right) throw std::runtime_error("Expected right side of 'math plus/minus' expression");
            auto op = op_str == "+" ? BinaryOpExpr::Op::Add : BinaryOpExpr::Op::Sub;
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }
        return left;
    }
    
    std::unique_ptr<Expression> parseMathMulDiv() {
        auto left = parseMathUnaryPlusMinus();
        if (!left) throw std::runtime_error("Expected left side of 'math mul/div' expression");

        static std::regex mul_div_tok(R"(\*\*?|//?|%(?!\}))");
        std::string op_str;
        while (!(op_str = consumeToken(mul_div_tok)).empty()) {
            auto right = parseMathUnaryPlusMinus();
            if (!right) throw std::runtime_error("Expected right side of 'math mul/div' expression");
            auto op = op_str == "*" ? BinaryOpExpr::Op::Mul 
                : op_str == "**" ? BinaryOpExpr::Op::MulMul 
                : op_str == "/" ? BinaryOpExpr::Op::Div 
                : op_str == "//" ? BinaryOpExpr::Op::DivDiv
                : BinaryOpExpr::Op::Mod;
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }

        if (!consumeToken("|").empty()) {
            auto expr = parseExpression();
            if (auto filter = dynamic_cast<FilterExpr*>(expr.get())) {
                filter->prepend(std::move(left));
                return expr;
            } else {
                std::vector<std::unique_ptr<Expression>> parts;
                parts.emplace_back(std::move(left));
                parts.emplace_back(std::move(expr));
                return nonstd_make_unique<FilterExpr>(std::move(parts));
            }
        }
        return left;
    }

    std::unique_ptr<Expression> call_func(const std::string & name, Expression::CallableArgs && args) const {
        return nonstd_make_unique<CallExpr>(nonstd_make_unique<VariableExpr>(name), std::move(args));
    }

    std::unique_ptr<FilterExpr> parseFilterExpression() {
        std::vector<std::unique_ptr<Expression>> parts;
        auto parseFunctionCall = [&]() {
            auto identifier = parseIdentifier();
            if (identifier.empty()) throw std::runtime_error("Expected identifier in filter expression");

            if (peekSymbols({ "(" })) {
                auto callParams = parseCallArgs();
                parts.push_back(call_func(identifier, std::move(callParams)));
            } else {
                parts.push_back(call_func(identifier, {}));
            }
        };
        parseFunctionCall();
        while (it != end && !consumeToken("|").empty()) {
            parseFunctionCall();
        }
        return nonstd_make_unique<FilterExpr>(std::move(parts));
    }

    std::unique_ptr<Expression> parseMathUnaryPlusMinus() {
        static std::regex unary_plus_minus_tok(R"(\+|-(?![}%#]\})|not)");
        auto op_str = consumeToken(unary_plus_minus_tok);
        auto expr = parseValueExpression();
        if (!expr) throw std::runtime_error("Expected expr of 'unary plus/minus' expression");
        
        if (!op_str.empty()) {
            auto op = op_str == "+" ? UnaryOpExpr::Op::Plus : op_str == "-" ? UnaryOpExpr::Op::Minus : UnaryOpExpr::Op::LogicalNot;
            return nonstd_make_unique<UnaryOpExpr>(std::move(expr), op);
        }
        return expr;
    }
        
    std::unique_ptr<Expression> parseValueExpression() {
      auto parseValue = [&]() -> std::unique_ptr<Expression> {
        auto constant = parseConstant();
        if (constant) return nonstd_make_unique<LiteralExpr>(*constant);

        static std::regex null_regex(R"(null\b)");
        if (!consumeToken(null_regex).empty()) return nonstd_make_unique<LiteralExpr>(Value());

        auto identifier = parseIdentifier();
        if (!identifier.empty()) return nonstd_make_unique<VariableExpr>(identifier);

        auto braced = parseBracedExpressionOrArray();
        if (braced) return braced;

        auto array = parseArray();
        if (array) return array;

        auto dictionary = parseDictionary();
        if (dictionary) return dictionary;

        throw std::runtime_error("Expected value expression");
      };

      auto value = parseValue();
      
      while (it != end && consumeSpaces() && peekSymbols({ "[", "." })) {
        if (!consumeToken("[").empty()) {
            std::unique_ptr<Expression> index;
            if (!consumeToken(":").empty()) {
              auto slice_end = parseExpression();
              index = nonstd_make_unique<SliceExpr>(nullptr, std::move(slice_end));
            } else {
              auto slice_start = parseExpression();
              if (!consumeToken(":").empty()) {
                consumeSpaces();
                if (peekSymbols({ "]" })) {
                  index = nonstd_make_unique<SliceExpr>(std::move(slice_start), nullptr);
                } else {
                  auto slice_end = parseExpression();
                  index = nonstd_make_unique<SliceExpr>(std::move(slice_start), std::move(slice_end));
                }
              } else {
                index = std::move(slice_start);
              }
            }
            if (!index) throw std::runtime_error("Empty index in subscript");
            if (consumeToken("]").empty()) throw std::runtime_error("Expected closing bracket in subscript");
            
            value = nonstd_make_unique<SubscriptExpr>(std::move(value), std::move(index));
        } else if (!consumeToken(".").empty()) {
            auto identifier = parseIdentifier();
            if (identifier.empty()) throw std::runtime_error("Expected identifier in subscript");

            consumeSpaces();
            if (peekSymbols({ "(" })) {
              auto callParams = parseCallArgs();
              value = nonstd_make_unique<MethodCallExpr>(std::move(value), identifier, std::move(callParams));
            } else {
              value = nonstd_make_unique<SubscriptExpr>(std::move(value), nonstd_make_unique<LiteralExpr>(Value(identifier)));
            }
        }
        consumeSpaces();
      }

      if (peekSymbols({ "(" })) {
        auto callParams = parseCallArgs();
        value = nonstd_make_unique<CallExpr>(std::move(value), std::move(callParams));
      }
      return value;
    }

    std::unique_ptr<Expression> parseBracedExpressionOrArray() {
        if (consumeToken("(").empty()) return nullptr;
        
        auto expr = parseExpression();
        if (!expr) throw std::runtime_error("Expected expression in braced expression");
        
        if (!consumeToken(")").empty()) {
            return expr;  // Drop the parentheses
        }

        std::vector<std::unique_ptr<Expression>> tuple;
        tuple.emplace_back(std::move(expr));

        while (it != end) {
          if (consumeToken(",").empty()) throw std::runtime_error("Expected comma in tuple");
          auto next = parseExpression();
          if (!next) throw std::runtime_error("Expected expression in tuple");
          tuple.push_back(std::move(next));

          if (!consumeToken(")").empty()) {
              return nonstd_make_unique<ArrayExpr>(std::move(tuple));
          }
        }
        throw std::runtime_error("Expected closing parenthesis");
    }

    std::unique_ptr<Expression> parseArray() {
        if (consumeToken("[").empty()) return nullptr;
        
        std::vector<std::unique_ptr<Expression>> elements;
        if (!consumeToken("]").empty()) {
            return nonstd_make_unique<ArrayExpr>(std::move(elements));
        }
        auto first_expr = parseExpression();
        if (!first_expr) throw std::runtime_error("Expected first expression in array");
        elements.push_back(std::move(first_expr));

        while (it != end) {
            if (!consumeToken(",").empty()) {
              auto expr = parseExpression();
              if (!expr) throw std::runtime_error("Expected expression in array");
              elements.push_back(std::move(expr));
            } else if (!consumeToken("]").empty()) {
                return nonstd_make_unique<ArrayExpr>(std::move(elements));
            } else {
                throw std::runtime_error("Expected comma or closing bracket in array");
            }
        }
        throw std::runtime_error("Expected closing bracket");
    }

    std::unique_ptr<Expression> parseDictionary() {
        if (consumeToken("{").empty()) return nullptr;
        
        std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> elements;
        if (!consumeToken("}").empty()) {
            return nonstd_make_unique<DictExpr>(std::move(elements));
        }

        auto parseKeyValuePair = [&]() {
            auto key = parseExpression();
            if (!key) throw std::runtime_error("Expected key in dictionary");
            if (consumeToken(":").empty()) throw std::runtime_error("Expected colon betweek key & value in dictionary");
            auto value = parseExpression();
            if (!value) throw std::runtime_error("Expected value in dictionary");
            elements.emplace_back(std::make_pair(std::move(key), std::move(value)));
        };

        parseKeyValuePair();
        
        while (it != end) {
            if (!consumeToken(",").empty()) {
                parseKeyValuePair();
            } else if (!consumeToken("}").empty()) {
                return nonstd_make_unique<DictExpr>(std::move(elements));
            } else {
                throw std::runtime_error("Expected comma or closing brace in dictionary");
            }
        }
        throw std::runtime_error("Expected closing brace");
    }

    static SpaceHandling parseSpaceHandling(const std::string& s) {
        if (s == "-") return SpaceHandling::Strip;
        return SpaceHandling::Keep;
    }

    using TemplateTokenVector = std::vector<std::unique_ptr<TemplateToken>>;
    using TemplateTokenIterator = TemplateTokenVector::const_iterator;

    std::vector<std::string> parseVarNames() {
      static std::regex varnames_regex(R"(((?:\w+)(?:[\n\s]*,[\n\s]*(?:\w+))*)[\n\s]*)");

      std::vector<std::string> group;
      if ((group = consumeTokenGroups(varnames_regex)).empty()) throw std::runtime_error("Expected variable names");
      std::vector<std::string> varnames;
      std::istringstream iss(group[1]);
      std::string varname;
      while (std::getline(iss, varname, ',')) {
        varnames.push_back(strip(varname));
      }
      return varnames;
    }

    std::runtime_error unexpected(const TemplateToken & token) const {
      return std::runtime_error("Unexpected " + TemplateToken::typeToString(token.type)
        + error_location_suffix(token.pos));
    }
    std::runtime_error unterminated(const TemplateToken & token) const {
      return std::runtime_error("Unterminated " + TemplateToken::typeToString(token.type)
        + error_location_suffix(token.pos));
    }

    /** Helper to get the n-th line (1-based) */
    std::string get_line(size_t line) const {
      auto start = template_str.begin();
      for (size_t i = 1; i < line; ++i) {
        start = std::find(start, template_str.end(), '\n') + 1;
      }
      auto end = std::find(start, template_str.end(), '\n');
      return std::string(start, end);
    }

    std::string error_location_suffix(size_t pos) const {
      auto start = template_str.begin();
      auto end = template_str.end();
      auto it = start + pos;
      auto line = std::count(start, it, '\n') + 1;
      auto max_line = std::count(start, end, '\n') + 1;
      auto col = pos - std::string(start, it).rfind('\n');
      std::ostringstream out;
      out << " at row " << line << ", column " << col << ":\n";
      if (line > 1) out << get_line(line - 1) << "\n";
      out << get_line(line) << "\n";
      out << std::string(col - 1, ' ') << "^" << "\n";
      if (line < max_line) out << get_line(line + 1) << "\n";

      return out.str();
    }

    TemplateTokenVector tokenize() {
      static std::regex comment_tok(R"(\{#([-~]?)(.*?)([-~]?)#\})");
      static std::regex expr_open_regex(R"(\{\{([-~])?)");
      static std::regex block_open_regex(R"(^\{%([-~])?[\s\n]*)");
      static std::regex block_keyword_tok(R"((if|else|elif|endif|for|endfor|set|endset|block|endblock|macro|endmacro)\b)");
      static std::regex text_regex(R"([\s\S\n]*?($|(?=\{\{|\{%|\{#)))");
      static std::regex expr_close_regex(R"([\s\n]*([-~])?\}\})");
      static std::regex block_close_regex(R"([\s\n]*([-~])?%\})");
              
      TemplateTokenVector tokens;
      std::vector<std::string> group;
      std::string text;
      
      try {
        while (it != end) {
          auto pos = std::distance(start, it);
      
          if (!(group = consumeTokenGroups(comment_tok, SpaceHandling::Keep)).empty()) {
            auto pre_space = parseSpaceHandling(group[1]);
            auto content = group[2];
            auto post_space = parseSpaceHandling(group[3]);
            tokens.push_back(nonstd_make_unique<CommentTemplateToken>(pos, pre_space, post_space, content));
          } else if (!(group = consumeTokenGroups(expr_open_regex, SpaceHandling::Keep)).empty()) {
            auto pre_space = parseSpaceHandling(group[1]);
            auto expr = parseExpression();

            if ((group = consumeTokenGroups(expr_close_regex)).empty()) {
              throw std::runtime_error("Expected closing expression tag");
            }

            auto post_space = parseSpaceHandling(group[1]);
            tokens.push_back(nonstd_make_unique<ExpressionTemplateToken>(pos, pre_space, post_space, std::move(expr)));
          } else if (!(group = consumeTokenGroups(block_open_regex, SpaceHandling::Keep)).empty()) {
            auto pre_space = parseSpaceHandling(group[1]);

            std::string keyword;

            auto parseBlockClose = [&]() -> SpaceHandling {
              if ((group = consumeTokenGroups(block_close_regex)).empty()) throw std::runtime_error("Expected closing block tag");
              return parseSpaceHandling(group[1]);
            };

            if ((keyword = consumeToken(block_keyword_tok)).empty()) throw std::runtime_error("Expected block keyword");
            
            if (keyword == "if") {
              auto condition = parseExpression();
              if (!condition) throw std::runtime_error("Expected condition in if block");

              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<IfTemplateToken>(pos, pre_space, post_space, std::move(condition)));
            } else if (keyword == "elif") {
              auto condition = parseExpression();
              if (!condition) throw std::runtime_error("Expected condition in elif block");

              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<ElifTemplateToken>(pos, pre_space, post_space, std::move(condition)));
            } else if (keyword == "else") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<ElseTemplateToken>(pos, pre_space, post_space));
            } else if (keyword == "endif") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<EndIfTemplateToken>(pos, pre_space, post_space));
            } else if (keyword == "for") {
              static std::regex recursive_tok(R"(recursive\b)");
              static std::regex if_tok(R"(if\b)");

              auto varnames = parseVarNames();
              static std::regex in_tok(R"(in\b)");
              if (consumeToken(in_tok).empty()) throw std::runtime_error("Expected 'in' keyword in for block");
              auto iterable = parseExpression(/* allow_if_expr = */ false);
              if (!iterable) throw std::runtime_error("Expected iterable in for block");

              std::unique_ptr<Expression> condition;
              if (!consumeToken(if_tok).empty()) {
                condition = parseExpression();
              }
              auto recursive = !consumeToken(recursive_tok).empty();
            
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<ForTemplateToken>(pos, pre_space, post_space, std::move(varnames), std::move(iterable), std::move(condition), recursive));
            } else if (keyword == "endfor") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<EndForTemplateToken>(pos, pre_space, post_space));
            } else if (keyword == "set") {
              static std::regex namespaced_var_regex(R"((\w+)[\s\n]*\.[\s\n]*(\w+))");
              if (!(group = consumeTokenGroups(namespaced_var_regex)).empty()) {
                auto ns = group[1];
                auto var = group[2]; 

                if (consumeToken("=").empty()) throw std::runtime_error("Expected equals sign in set block");

                auto value = parseExpression();
                if (!value) throw std::runtime_error("Expected value in set block");

                auto post_space = parseBlockClose();
                tokens.push_back(nonstd_make_unique<NamespacedSetTemplateToken>(pos, pre_space, post_space, ns, var, std::move(value)));
              } else {
                auto varnames = parseVarNames();

                if (!consumeToken("=").empty()) {
                  auto value = parseExpression();
                  if (!value) throw std::runtime_error("Expected value in set block");

                  auto post_space = parseBlockClose();
                  tokens.push_back(nonstd_make_unique<SetTemplateToken>(pos, pre_space, post_space, varnames, std::move(value)));
                } else {
                  auto post_space = parseBlockClose();
                  tokens.push_back(nonstd_make_unique<SetTemplateToken>(pos, pre_space, post_space, varnames, nullptr));
                }
              }
            } else if (keyword == "endset") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<EndSetTemplateToken>(pos, pre_space, post_space));
            } else if (keyword == "block") {
              auto blockname = parseIdentifier();
              if (blockname.empty()) throw std::runtime_error("Expected block name in block block");

              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<BlockTemplateToken>(pos, pre_space, post_space, blockname));
            } else if (keyword == "endblock") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<EndBlockTemplateToken>(pos, pre_space, post_space));
            } else if (keyword == "macro") {
              auto macroname = parseIdentifier();
              if (macroname.empty()) throw std::runtime_error("Expected macro name in macro block");
              auto params = parseParameters();

              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<MacroTemplateToken>(pos, pre_space, post_space, macroname, std::move(params)));
            } else if (keyword == "endmacro") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<EndMacroTemplateToken>(pos, pre_space, post_space));
            } else {
              throw std::runtime_error("Unexpected block: " + keyword);
            }
          } else if (!(text = consumeToken(text_regex, SpaceHandling::Keep)).empty()) {
            tokens.push_back(nonstd_make_unique<TextTemplateToken>(pos, SpaceHandling::Keep, SpaceHandling::Keep, text));
          } else {
            if (it != end) throw std::runtime_error("Unexpected character");
          }
        }
        return tokens;
      } catch (const std::runtime_error & e) {
        throw std::runtime_error(e.what() + error_location_suffix(std::distance(start, it)));
      }
    }

    std::unique_ptr<TemplateNode> parseTemplate(
          const TemplateTokenIterator & begin,
          TemplateTokenIterator & it,
          const TemplateTokenIterator & end,
          bool fully = false) const {
        std::vector<std::unique_ptr<TemplateNode>> children;
        while (it != end) {
          const auto start = it;
          const auto & token = *(it++);
          if (auto if_token = dynamic_cast<IfTemplateToken*>(token.get())) {
              std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> cascade;
              cascade.emplace_back(std::move(if_token->condition), parseTemplate(begin, it, end));

              while (it != end && (*it)->type == TemplateToken::Type::Elif) {
                  auto elif_token = dynamic_cast<ElifTemplateToken*>((*(it++)).get());
                  cascade.emplace_back(std::move(elif_token->condition), parseTemplate(begin, it, end));
              }

              if (it != end && (*it)->type == TemplateToken::Type::Else) {
                cascade.emplace_back(nullptr, parseTemplate(begin, ++it, end));
              }
              if (it == end || (*(it++))->type != TemplateToken::Type::EndIf) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<IfNode>(std::move(cascade)));
          } else if (auto for_token = dynamic_cast<ForTemplateToken*>(token.get())) {
              auto body = parseTemplate(begin, it, end);
              auto else_body = std::unique_ptr<TemplateNode>();
              if (it != end && (*it)->type == TemplateToken::Type::Else) {
                else_body = parseTemplate(begin, ++it, end);
              }
              if (it == end || (*(it++))->type != TemplateToken::Type::EndFor) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<ForNode>(for_token->var_names, std::move(for_token->iterable), std::move(for_token->condition), std::move(body), for_token->recursive, std::move(else_body)));
          } else if (auto text_token = dynamic_cast<TextTemplateToken*>(token.get())) {
              SpaceHandling pre_space = (it - 1) != begin ? (*(it - 2))->post_space : SpaceHandling::Keep;
              SpaceHandling post_space = it != end ? (*it)->pre_space : SpaceHandling::Keep;
              auto text = text_token->text;
              if (pre_space == SpaceHandling::Strip) {
                static std::regex leading_space_regex(R"(^(\s|\r|\n)+)");
                text = std::regex_replace(text, leading_space_regex, "");
              }
              if (post_space == SpaceHandling::Strip) {
                static std::regex trailing_space_regex(R"((\s|\r|\n)+$)");
                text = std::regex_replace(text, trailing_space_regex, "");
              }
              if (it == end) {
                static std::regex r(R"([\n\r]$)");
                text = std::regex_replace(text, r, "");  // Strip one trailing newline
              }
              children.emplace_back(nonstd_make_unique<TextNode>(text));
          } else if (auto expr_token = dynamic_cast<ExpressionTemplateToken*>(token.get())) {
              children.emplace_back(nonstd_make_unique<ExpressionNode>(std::move(expr_token->expr)));
          } else if (auto set_token = dynamic_cast<SetTemplateToken*>(token.get())) {
            if (set_token->value) {
              children.emplace_back(nonstd_make_unique<SetNode>(set_token->var_names, std::move(set_token->value), nullptr));
            } else {
              auto value_template = parseTemplate(begin, it, end);
              if (it == end || (*(it++))->type != TemplateToken::Type::EndSet) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<SetNode>(set_token->var_names, nullptr, std::move(value_template)));
            }
          } else if (auto namespaced_set_token = dynamic_cast<NamespacedSetTemplateToken*>(token.get())) {
              children.emplace_back(nonstd_make_unique<NamespacedSetNode>(namespaced_set_token->ns, namespaced_set_token->name, std::move(namespaced_set_token->value)));
          } else if (auto block_token = dynamic_cast<BlockTemplateToken*>(token.get())) {
              auto body = parseTemplate(begin, it, end);
              if (it == end || (*(it++))->type != TemplateToken::Type::EndBlock) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<BlockNode>(block_token->name, std::move(body)));
          } else if (auto macro_token = dynamic_cast<MacroTemplateToken*>(token.get())) {
              auto body = parseTemplate(begin, it, end);
              if (it == end || (*(it++))->type != TemplateToken::Type::EndMacro) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<MacroNode>(macro_token->name, std::move(macro_token->params), std::move(body)));
          } else if (auto comment_token = dynamic_cast<CommentTemplateToken*>(token.get())) {
              // Ignore comments
          } else if (dynamic_cast<EndBlockTemplateToken*>(token.get())
                  || dynamic_cast<EndForTemplateToken*>(token.get())
                  || dynamic_cast<EndSetTemplateToken*>(token.get())
                  || dynamic_cast<EndMacroTemplateToken*>(token.get())
                  || dynamic_cast<EndIfTemplateToken*>(token.get())
                  || dynamic_cast<ElseTemplateToken*>(token.get())
                  || dynamic_cast<ElifTemplateToken*>(token.get())) {
              it--;  // unconsume the token
              break;  // exit the loop
          } else {
              throw unexpected(**(it-1));
          }
        }
        if (fully && it != end) {
            throw unexpected(**it);
        }
        if (children.empty()) {
          return nonstd_make_unique<TextNode>("");
        } else if (children.size() == 1) {
          return std::move(children[0]);
        } else {
          return nonstd_make_unique<SequenceNode>(std::move(children));
        }
    }

public:

    static std::unique_ptr<TemplateNode> parse(const std::string& template_str) {
        Parser parser(template_str);

        auto tokens = parser.tokenize();
        TemplateTokenIterator begin = tokens.begin();
        auto it = begin;
        TemplateTokenIterator end = tokens.end();
        return parser.parseTemplate(begin, it, end, /* full= */ true);
    }
};

static Value simple_function(const std::string & fn_name, const std::vector<std::string> & params, const std::function<Value(Context &, const Value & args)> & fn) {
  std::map<std::string, size_t> named_positions;
  for (size_t i = 0, n = params.size(); i < n; i++) named_positions[params[i]] = i;

  return Value::callable([=](Context & context, const Value::CallableArgs & args) -> Value {
    auto args_obj = Value::object();
    auto positional = true;
    std::vector<bool> provided_args(params.size());
    for (size_t i = 0, n = args.args.size(); i < n; i++) {
      auto & arg = args.args[i];
      if (i < params.size()) {
        args_obj.set(params[i], arg);
        provided_args[i] = true;
      } else {
        throw std::runtime_error(("Too many positional params for " + fn_name + ": ") + args.dump().dump());
      }
    }
    for (size_t i = 0, n = args.kwargs.size(); i < n; i++) {
      auto & arg = args.kwargs[i];
      auto named_pos_it = named_positions.find(arg.first);
      if (named_pos_it == named_positions.end()) {
        throw std::runtime_error("Unknown argument " + arg.first + " for function " + fn_name);
      }
      provided_args[named_pos_it->second] = true;
      args_obj.set(arg.first, arg.second);
    }
    return fn(context, args_obj);
  });
}

std::shared_ptr<Context> Context::builtins() {
  auto top_level_values = Value::object();

  top_level_values.set("raise_exception", simple_function("raise_exception", { "message" }, [](Context &, const Value & args) -> Value {
    throw std::runtime_error(args.at("message").get<std::string>());
  }));
  top_level_values.set("tojson", simple_function("tojson", { "value", "indent" }, [](Context &, const Value & args) {
    return Value(args.at("value").dump(args.get<int64_t>("indent", -1)));
  }));
  top_level_values.set("items", simple_function("items", { "object" }, [](Context &, const Value & args) {
    auto items = Value::array();
    if (args.contains("object")) {
      auto & obj = args.at("object");
      if (!obj.is_null()) {
        for (auto & key : obj.keys()) {
          items.push_back(Value::array({key, obj.at(key)}));
        }
      }
    }
    return items;
  }));
  top_level_values.set("trim", simple_function("trim", { "text" }, [](Context &, const Value & args) {
    auto & text = args.at("text");
    return text.is_null() ? text : Value(strip(text.get<std::string>()));
  }));
  auto escape = simple_function("escape", { "text" }, [](Context &, const Value & args) {
    return Value(html_escape(args.at("text").get<std::string>()));
  });
  top_level_values.set("e", escape);
  top_level_values.set("escape", escape);
  top_level_values.set("joiner", simple_function("joiner", { "sep" }, [](Context &, const Value & args) {
    auto sep = args.get<std::string>("sep", "");
    auto first = std::make_shared<bool>(true);
    return simple_function("", {}, [sep, first](Context &, const Value & args) -> Value {
      if (*first) {
        *first = false;
        return "";
      }
      return sep;
    });
    return Value(html_escape(args.at("text").get<std::string>()));
  }));
  top_level_values.set("count", simple_function("count", { "items" }, [](Context &, const Value & args) {
    return Value((int64_t) args.at("items").size());
  }));
  top_level_values.set("dictsort", simple_function("dictsort", { "value" }, [](Context &, const Value & args) {
    if (args.size() != 1) throw std::runtime_error("dictsort expects exactly 1 argument (TODO: fix implementation)");
    auto & value = args.at("value");
    auto keys = value.keys();
    std::sort(keys.begin(), keys.end());
    auto res = Value::array();
    for (auto & key : keys) {
      res.push_back(Value::array({key, value.at(key)}));
    }
    return res;
  }));
  top_level_values.set("join", simple_function("join", { "items", "d" }, [](Context &, const Value & args) {
    auto do_join = [](const Value & items, const std::string & sep) {
      std::ostringstream oss;
      auto first = true;
      for (size_t i = 0, n = items.size(); i < n; ++i) {
        if (first) first = false;
        else oss << sep;
        oss << items.at(i).to_str();
      }
      return Value(oss.str());
    };
    auto sep = args.get<std::string>("d", "");
    if (args.contains("items")) {
        auto & items = args.at("items");
        return do_join(items, sep);
    } else {
      return simple_function("", {"items"}, [sep, do_join](Context &, const Value & args) {
        auto & items = args.at("items");
        if (!items || !items.is_array()) throw std::runtime_error("join expects an array for items, got: " + items.dump());
        return do_join(items, sep);
      });
    }
  }));
  top_level_values.set("namespace", Value::callable([=](Context &, const Value::CallableArgs & args) {
    auto ns = Value::object();
    args.expectArgs("namespace", {0, 0}, {0, std::numeric_limits<size_t>::max()});
    for (auto & arg : args.kwargs) {
      ns.set(arg.first, arg.second);
    }
    return ns;
  }));
  top_level_values.set("equalto", simple_function("equalto", { "expected", "actual" }, [](Context &, const Value & args) -> Value {
      return args.at("actual") == args.at("expected");
  }));
  top_level_values.set("length", simple_function("length", { "items" }, [](Context &, const Value & args) -> Value {
      return (int64_t) args.at("items").size();
  }));
  top_level_values.set("list", simple_function("list", { "items" }, [](Context &, const Value & args) -> Value {
      auto items = args.at("items");
      if (!items.is_array()) throw std::runtime_error("object is not iterable");
      return items;
  }));
  top_level_values.set("unique", simple_function("unique", { "items" }, [](Context &, const Value & args) -> Value {
      auto items = args.at("items");
      if (!items.is_array()) throw std::runtime_error("object is not iterable");
      std::unordered_set<Value> seen;
      auto result = Value::array();
      for (size_t i = 0, n = items.size(); i < n; i++) {
        auto pair = seen.insert(items.at(i));
        if (pair.second) {
          result.push_back(items.at(i));
        }
      }
      return result;
  }));
  auto make_filter = [](const Value & filter, const Value & extra_args) -> Value {
    return simple_function("", { "value" }, [=](Context & context, const Value & args) {
      auto value = args.at("value");
      Value::CallableArgs actual_args;
      actual_args.args.emplace_back(value);
      for (size_t i = 0, n = extra_args.size(); i < n; i++) {
        actual_args.args.emplace_back(extra_args.at(i));
      }
      return filter.call(context, actual_args);
    });
  };
  // https://jinja.palletsprojects.com/en/3.0.x/templates/#jinja-filters.reject
  top_level_values.set("reject", Value::callable([=](Context & context, const Value::CallableArgs & args) {
    args.expectArgs("reject", {2, std::numeric_limits<size_t>::max()}, {0, 0});
    auto & items = args.args[0];
    auto filter_fn = context.get(args.args[1]);
    if (!filter_fn) throw std::runtime_error("Function not found " + args.args[1].dump());

    auto filter_args = Value::array();
    for (size_t i = 2, n = args.args.size(); i < n; i++) {
      filter_args.push_back(args.args[i]);
    }
    auto filter = make_filter(filter_fn, filter_args);

    auto res = Value::array();
    for (size_t i = 0, n = items.size(); i < n; i++) {
      auto & item = items.at(i);
      auto pred_res = filter.call(context, { { "", item } });
      if (!pred_res) {
        res.push_back(item);
      }
    }
    return res;
  }));
  top_level_values.set("range", Value::callable([=](Context &, const Value::CallableArgs & args) {
    std::vector<int64_t> startEndStep(3);
    std::vector<bool> param_set(3);
    if (args.args.size() == 1) {
      startEndStep[1] = args.args[0].get<int64_t>();
      param_set[1] = true;
    } else {
      for (size_t i = 0; i < args.args.size(); i++) {
        auto & arg = args.args[i];
        auto v = arg.get<int64_t>();
        startEndStep[i] = v;
        param_set[i] = true;
        }
      }
      for (auto & arg : args.kwargs) {
        size_t i;
        if (arg.first == "start") i = 0;
        else if (arg.first == "end") i = 1;
        else if (arg.first == "step") i = 2;
        else throw std::runtime_error("Unknown argument " + arg.first + " for function range");

        if (param_set[i]) {
          throw std::runtime_error("Duplicate argument " + arg.first + " for function range");
        }
        startEndStep[i] = arg.second.get<int64_t>();
        param_set[i] = true;
    }
    if (!param_set[1]) {
      throw std::runtime_error("Missing required argument 'end' for function range");
    }
    int64_t start = param_set[0] ? startEndStep[0] : 0;
    int64_t end = startEndStep[1];
    int64_t step = param_set[2] ? startEndStep[2] : 1;
    
    auto res = Value::array();
    if (step > 0) {
      for (int64_t i = start; i < end; i += step) {
        res.push_back(Value(i));
      }
    } else {
      for (int64_t i = start; i > end; i += step) {
        res.push_back(Value(i));
      }
    }
    return res;
  }));

  return std::make_shared<Context>(std::move(top_level_values));
}

std::shared_ptr<Context> Context::make(Value && values) {
  return std::make_shared<Context>(values.is_null() ? Value::object() : std::move(values), builtins());
}

json Context::dump() const {
  json res = parent_ ? parent_->dump() : json::object();
  for (auto & key : values_.keys()) {
    res[key.get<std::string>()] = values_.at(key).get<json>();
  }
  return res;
}

}  // namespace jinja
