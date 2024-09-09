/*
  The absolute minimum needed to run chat templates.
  As it turns out, Llama 3.1's template is relatively involved, so we need a proper template engine.

  TODO:
  - Add loop.index, .first and friends https://jinja.palletsprojects.com/en/3.0.x/templates/#for
  - Add more tests
  - Add more functions
    - https://jinja.palletsprojects.com/en/3.0.x/templates/#builtin-filters
    - https://jbmoelker.github.io/jinja-compat-tests/
      - `{% raw %}{{ broken }{% endraw %}` https://jbmoelker.github.io/jinja-compat-tests/tags/raw/
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
#include <json.hpp>

using json = nlohmann::ordered_json;

/* Backport make_unique from C++14. */
template <class T, class... Args>
typename std::unique_ptr<T> nonstd_make_unique(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

namespace jinja {

/* Values that behave roughly like in Python.
 * jinja templates deal with objects by reference so we can't just json for arrays & objects,
 * but we do for primitives.
 */
class Value : public std::enable_shared_from_this<Value> {
public:
  using CallableArgs = std::vector<std::pair<std::string, Value>>;
  using CallableType = std::function<Value(const Value &, const CallableArgs &)>;

private:
  using ObjectType = std::unordered_map<json, Value>;  // Only contains primitive keys
  using ArrayType = std::vector<Value>;

  std::shared_ptr<ArrayType> array_;
  std::shared_ptr<ObjectType> object_;
  std::shared_ptr<CallableType> callable_;
  json primitive_;

  Value(const std::shared_ptr<ArrayType> & array) : array_(array) {}
  Value(const std::shared_ptr<ObjectType> & object) : object_(object) {}
  Value(const std::shared_ptr<CallableType> & callable) : callable_(callable), object_(std::make_shared<ObjectType>()) {}

  void dump(std::ostringstream & out, int indent = -1, int level = 0) const {
    auto print_indent = [&](int level) {
      if (indent > 0) {
        // out << std::string(level * indent, ' ');
        for (int i = 0, n = level * indent; i < n; ++i) out << ' ';
      }
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
    } else {
      out << primitive_.dump();
    }
  }

public:
  Value() {}
  Value(const bool& v) : primitive_(v) {}
  Value(const int64_t& v) : primitive_(v) {}
  Value(const double& v) : primitive_(v) {}
  Value(const nullptr_t& v) {}
  Value(const std::string& v) : primitive_(v) {}
  Value(const char * v) : primitive_(std::string(v)) {}

  Value(const json& v) {
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
  
  Value keys() const {
    if (!object_) throw std::runtime_error("Value is not an object: " + dump());

    auto res = Value::array();
    for (const auto& item : *object_) {
      res.push_back(item.first);
    }
    return res;
  }

  size_t size() const {
    if (!is_array()) throw std::runtime_error("Value is not an array: " + dump());
    return array_->size();
  }

  static Value array(const std::shared_ptr<ArrayType> array = std::make_shared<ArrayType>()) {
    return Value(array);
  }
  static Value object(const std::shared_ptr<ObjectType> object = std::make_shared<ObjectType>()) {
    return Value(object);
  }
  static Value callable(const CallableType & callable) {
    return Value(std::make_shared<CallableType>(callable));
  }
  static Value context(const Value & values = {});

  void push_back(const Value& v) {
    if (!array_) throw std::runtime_error("Value is not an array: " + dump());
    array_->push_back(v);
  }

  Value get(const Value& key) const {
    if (array_) {
      return array_->at(key.get<int>());
    } else if (object_) {
      if (!key.is_hashable()) throw std::runtime_error("Unashable type: " + dump());
      return object_->at(key.primitive_);
    }
    throw std::runtime_error("Value is not an array or object: " + dump());
  }

  void set(const Value& key, const Value& value) {
    if (!is_object()) throw std::runtime_error("Value is not an object: " + dump());
    if (!key.is_hashable()) throw std::runtime_error("Unashable type: " + dump());
    (*object_)[key.primitive_] = value;
  }

  Value call(const Value & context, const CallableArgs & args) const {
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
    if (is_null()) throw std::runtime_error("Undefined value or reference");
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

  bool operator==(const Value & other) const {
    if (is_callable() || other.is_callable()) {
      if (callable_.get() != other.callable_.get()) return false;
    }
    if (is_array()) {
      if (!other.is_array()) return false;
      if (array_->size() != other.array_->size()) return false;
      for (size_t i = 0; i < array_->size(); ++i) {
        if (!(*array_)[i] || !(*other.array_)[i] || (*array_)[i] != (*other.array_)[i]) return false;
      }
      return true;
    } else if (is_object()) {
      if (!other.is_object()) return false;
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
    if (is_null()) throw std::runtime_error("Undefined value or reference");
    if (array_) {
      for (const auto& item : *array_) {
        if (item && item == value) return true;
      }
      return false;
    } else if (object_) {
      for (const auto& item : *object_) {
        if (item.second == value) return true;
      }
      return false;
    } else {
      throw std::runtime_error("contains can only be called on arrays and objects: " + dump());
    }
  }
  void erase(const std::string & key) {
    if (object_) throw std::runtime_error("Value is not an object: " + dump());
    object_->erase(key);
  }
  const Value& at(size_t index) const {
    if (is_null()) throw std::runtime_error("Undefined value or reference");
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
    throw std::runtime_error("Get not defined for this value type");
  }

  template <>
  std::vector<std::string> get<std::vector<std::string>>() const {
    if (is_array()) {
      std::vector<std::string> res;
      for (const auto& item : *array_) {
        res.push_back(item.get<std::string>());
      }
      return res;
    }
    throw std::runtime_error("Get not defined for this value type");
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
    if (is_array()) {
      std::vector<json> res;
      for (const auto& item : *array_) {
        res.push_back(item.get<json>());
      }
      return res;
    }
    if (is_object()) {
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
    throw std::runtime_error("Get not defined for this value type");
  }

  Value operator-() const {
      if (is_number_integer())
        return -get<int64_t>();
      else
        return -get<double>();
  }
  Value operator+(const Value& rhs) {
      if (is_string() || rhs.is_string())
        return get<std::string>() + rhs.get<std::string>();
      else if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() + rhs.get<int64_t>();
      else
        return get<double>() + rhs.get<double>();
  }
  Value operator-(const Value& rhs) {
      if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() - rhs.get<int64_t>();
      else
        return get<double>() - rhs.get<double>();
  }
  Value operator*(const Value& rhs) {
      if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() * rhs.get<int64_t>();
      else
        return get<double>() * rhs.get<double>();
  }
  Value operator/(const Value& rhs) {
      if (is_number_integer() && rhs.is_number_integer())
        return get<int64_t>() / rhs.get<int64_t>();
      else
        return get<double>() / rhs.get<double>();
  }
  Value operator%(const Value& rhs) {
    return get<int64_t>() % rhs.get<int64_t>();
  }
};

class Expression {
public:
    using CallableArgs = std::vector<std::pair<std::string, std::unique_ptr<Expression>>>;

    virtual ~Expression() = default;
    virtual Value evaluate(const Value & context) const = 0;
    virtual Value evaluateAsPipe(const Value & context, Value&) const{
      throw std::runtime_error("This expression cannot be used as a pipe");
    }
};

static void destructuring_assign(const std::vector<std::string> & var_names, Value & context, const Value& item) {
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
    enum class Type { Text, Expression, If, Else, Elif, EndIf, For, EndFor, Set, NamespacedSet, Comment, Block, EndBlock };

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
            case Type::NamespacedSet: return "namespaced set";
            case Type::Comment: return "comment";
            case Type::Block: return "block";
            case Type::EndBlock: return "endblock";
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
    virtual void render(std::ostringstream& oss, Value & context) const = 0;
    std::string render(Value & context) const {
        std::ostringstream oss;
        render(oss, context);
        return oss.str();
    }
};

class SequenceNode : public TemplateNode {
    std::vector<std::unique_ptr<TemplateNode>> children;
public:
    SequenceNode(std::vector<std::unique_ptr<TemplateNode>> && c) : children(std::move(c)) {}
    void render(std::ostringstream& oss, Value & context) const override {
        for (const auto& child : children) child->render(oss, context);
    }
};

class TextNode : public TemplateNode {
    std::string text;
public:
    TextNode(const std::string& t) : text(t) {}
    void render(std::ostringstream& oss, Value &) const override { oss << text; }
};

class VariableNode : public TemplateNode {
    std::unique_ptr<Expression> expr;
public:
    VariableNode(std::unique_ptr<Expression> && e, std::vector<std::string> && f) 
        : expr(std::move(e)) {}
    void render(std::ostringstream& oss, Value & context) const override {
      auto result = expr->evaluate(context);
      if (result.is_string()) {
          oss << result.get<std::string>();
      } else if (result.is_boolean()) {
          oss << (result.get<bool>() ? "True" : "False");
      } else if (!result.is_null()) {
          oss << result.dump();
      }
  }
};

class IfNode : public TemplateNode {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> cascade;
public:
    IfNode(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> && c)
        : cascade(std::move(c)) {}
    void render(std::ostringstream& oss, Value & context) const override {
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
};

class ForNode : public TemplateNode {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> iterable;
    std::unique_ptr<Expression> condition;
    std::unique_ptr<TemplateNode> body;
    bool recursive;
public:
    ForNode(const std::vector<std::string> & vns, std::unique_ptr<Expression> && iter,
      std::unique_ptr<Expression> && c,
            std::unique_ptr<TemplateNode> && b, bool r)
            : var_names(vns), condition(std::move(c)),
            iterable(std::move(iter)), body(std::move(b)), recursive(r) {}
    void render(std::ostringstream& oss, Value & context) const override {
      auto iterable_value = iterable->evaluate(context);
      if (!iterable_value.is_array()) {
        throw std::runtime_error("For loop iterable must be iterable");
      }

      auto original_context = context;
      auto original_vars = Value::object();
      for (const auto& var_name : var_names) {
          original_vars.set(var_name, context.contains(var_name) ? context.at(var_name) : Value());
      }

      auto loop_iteration = [&](const Value& item) {
          destructuring_assign(var_names, context, item);
          if (!condition || condition->evaluate(context)) {
            body->render(oss, context);
          }
      };
      std::function<void(const Value&)> visit = [&](const Value& iter) {
          for (size_t i = 0, n = iter.size(); i < n; ++i) {
              auto & item = iter.at(i);
              if (item.is_array() && recursive) {
                  visit(item);
              } else {
                  loop_iteration(item);
              }
          }
      };
      visit(iterable_value);
      
      for (const auto& var_name : var_names) {
          if (original_vars.contains(var_name)) {
              context.set(var_name, original_vars.at(var_name));
          } else {
              context.erase(var_name);
          }
      }
  }
};

class BlockNode : public TemplateNode {
    std::string name;
    std::unique_ptr<TemplateNode> body;
public:
    BlockNode(const std::string& n, std::unique_ptr<TemplateNode> && b)
        : name(n), body(std::move(b)) {}
    void render(std::ostringstream& oss, Value & context) const override {
        body->render(oss, context);
    }
};

class SetNode : public TemplateNode {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> value;
public:
    SetNode(const std::vector<std::string> & vns, std::unique_ptr<Expression> && v)
      : var_names(vns), value(std::move(v)) {}
    void render(std::ostringstream& oss, Value & context) const override {
        destructuring_assign(var_names, context, value->evaluate(context));
    }
};

class NamespacedSetNode : public TemplateNode {
    std::string ns, name;
    std::unique_ptr<Expression> value;
public:
    NamespacedSetNode(const std::string& ns, const std::string& name, std::unique_ptr<Expression> && v)
      : ns(ns), name(name), value(std::move(v)) {}
    void render(std::ostringstream& oss, Value & context) const override {
        auto ns_value = context.get(ns);
        if (!ns_value.is_object()) throw std::runtime_error("Namespace '" + ns + "' is not an object");
        ns_value.set(name, this->value->evaluate(context));
    }
};

class IfExpr : public Expression {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> then_expr;
    std::unique_ptr<Expression> else_expr;
public:
    IfExpr(std::unique_ptr<Expression> && c, std::unique_ptr<Expression> && t, std::unique_ptr<Expression> && e)
        : condition(std::move(c)), then_expr(std::move(t)), else_expr(std::move(e)) {}
    Value evaluate(const Value & context) const override {
        return condition->evaluate(context) ? then_expr->evaluate(context) : else_expr->evaluate(context);
    }
};

class LiteralExpr : public Expression {
    Value value;
public:
    LiteralExpr(const Value& v) : value(v) {}
    Value evaluate(const Value &) const override { return value; }
};

class ArrayExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> elements;
public:
    ArrayExpr(std::vector<std::unique_ptr<Expression>> && e) : elements(std::move(e)) {}
    Value evaluate(const Value & context) const override {
        auto result = Value::array();
        for (const auto& e : elements) {
            result.push_back(e->evaluate(context));
        }
        return result;
    }
};

class DictExpr : public Expression {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> elements;
public:
    DictExpr(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> && e) : elements(std::move(e)) {}
    Value evaluate(const Value & context) const override {
        auto result = Value::object();
        for (const auto& e : elements) {
            result.set(e.first->evaluate(context), e.second->evaluate(context));
        }
        return result;
    }
};

class VariableExpr : public Expression {
    std::string name;
public:
    VariableExpr(const std::string& n) : name(n) {}
    std::string get_name() const { return name; }
    Value evaluate(const Value & context) const override {
        if (!context.contains(name)) {
            std::cerr << "Failed to find '" << name << "' in context (has keys: " << context.keys().dump() << ")" << std::endl;
            return Value();
        }
        return context.at(name);
    }
};

class SliceExpr : public Expression {
public:
    std::unique_ptr<Expression> start, end;
    SliceExpr(std::unique_ptr<Expression> && s, std::unique_ptr<Expression> && e) : start(std::move(s)), end(std::move(e)) {}
    Value evaluate(const Value & context) const override {
        throw std::runtime_error("SliceExpr not implemented");
    }
};

class SubscriptExpr : public Expression {
    std::unique_ptr<Expression> base;
    std::unique_ptr<Expression> index;
public:
    SubscriptExpr(std::unique_ptr<Expression> && b, std::unique_ptr<Expression> && i)
        : base(std::move(b)), index(std::move(i)) {}
    Value evaluate(const Value & context) const override {
        auto target_value = base->evaluate(context);
        if (auto slice = dynamic_cast<SliceExpr*>(index.get())) {
          if (!target_value.is_array()) throw std::runtime_error("Subscripting non-array");

          auto start = slice->start ? slice->start->evaluate(context).get<int>() : 0;
          auto end = slice->end ? slice->end->evaluate(context).get<int>() : target_value.size();
          auto result = Value::array();
          for (int i = start; i < end; ++i) {
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
          if (target_value.is_array()) {
              return target_value.at(index_value.get<int>());
          } else if (target_value.is_object()) {
              auto key = index_value.get<std::string>();
              if (!target_value.contains(key)) {
                  throw std::runtime_error("'dict object' has no attribute '" + key + "'");
              }
              return target_value.at(key);
          } else {
              throw std::runtime_error("Subscripting non-array or non-object");
          }
        }
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
    Value evaluate(const Value & context) const override {
        auto e = expr->evaluate(context);
        switch (op) {
            case Op::Plus: return e;
            case Op::Minus: return -e;
            case Op::LogicalNot: return !e;
        }
        throw std::runtime_error("Unknown unary operator");
    }
};

class BinaryOpExpr : public Expression {
public:
    enum class Op { StrConcat, Add, Sub, Mul, MulMul, Div, DivDiv, Mod, Eq, Ne, Lt, Gt, Le, Ge, And, Or, In, Is, IsNot };
private:
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    Op op;
public:
    BinaryOpExpr(std::unique_ptr<Expression> && l, std::unique_ptr<Expression> && r, Op o)
        : left(std::move(l)), right(std::move(r)), op(o) {}
    Value evaluate(const Value & context) const override {
        auto l = left->evaluate(context);
        
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

        auto r = right->evaluate(context);
        switch (op) {
            case Op::StrConcat: return Value(l.get<std::string>() + r.get<std::string>());
            case Op::Add:       return l + r;
            case Op::Sub:       return l - r;
            case Op::Mul:       return l * r;
            case Op::Div:       return l / r;
            case Op::MulMul:    return Value(std::pow(l.get<double>(), r.get<double>()));
            case Op::DivDiv:    return Value(l.get<int64_t>() / r.get<int64_t>());
            case Op::Mod:       return Value(l.get<int64_t>() % r.get<int64_t>());
            case Op::Eq:        return Value(l == r);
            case Op::Ne:        return Value(l != r);
            case Op::Lt:        return Value(l < r);
            case Op::Gt:        return Value(l > r);
            case Op::Le:        return Value(l <= r);
            case Op::Ge:        return Value(l >= r);
            case Op::And:       return Value(l && r);
            case Op::Or:        return Value(l || r);
            case Op::In:        return Value(r.is_array() && r.contains(l));
            default:            break;
        }
        throw std::runtime_error("Unknown binary operator");
    }
};

class MethodCallExpr : public Expression {
    std::unique_ptr<Expression> object;
    std::string method;
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> args;
public:
    MethodCallExpr(std::unique_ptr<Expression> && obj, const std::string& m, std::vector<std::pair<std::string, std::unique_ptr<Expression>>> && a)
        : object(std::move(obj)), method(m), args(std::move(a)) {}
    const std::string& get_method() const { return method; }
    Value evaluate(const Value & context) const override {
        auto obj = object->evaluate(context);
        if (method == "append" && obj.is_array()) {
            if (args.size() != 1 || !args[0].first.empty()) throw std::runtime_error("append method must have exactly one unnamed argument");
            obj.push_back(args[0].second->evaluate(context));
            return Value();
        }
        throw std::runtime_error("Unknown method: " + method);
    }
};

class CallExpr : public Expression {
public:
    std::unique_ptr<Expression> object;
    Expression::CallableArgs args;
    CallExpr(std::unique_ptr<Expression> && obj, Expression::CallableArgs && a)
        : object(std::move(obj)), args(std::move(a)) {}
    Value evaluate(const Value & context) const override {
        auto obj = object->evaluate(context);
        if (!obj.is_callable()) {
          throw std::runtime_error("Object is not callable: " + obj.dump(2));
        }
        Value::CallableArgs vargs;
        for (const auto& arg : args) {
            vargs.push_back({arg.first, arg.second->evaluate(context)});
        }
        return obj.call(context, vargs);
    }

    Value evaluateAsPipe(const Value & context, Value& input) const override {
        auto obj = object->evaluate(context);
        if (obj.is_null()) {
          throw std::runtime_error("Object is null, cannot be called");
        }
        if (!obj.is_callable()) {
          throw std::runtime_error("Object is not callable: " + obj.dump(2));
        }
        Value::CallableArgs vargs;
        vargs.push_back({"", input});
        for (const auto& arg : args) {
            vargs.push_back({arg.first, arg.second->evaluate(context)});
        }
        return obj.call(context, vargs);
    }
};

class FilterExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> parts;
public:
    FilterExpr(std::vector<std::unique_ptr<Expression>> && p) : parts(std::move(p)) {}
    Value evaluate(const Value & context) const override {
        Value result;
        bool first = true;
        for (const auto& part : parts) {
          if (first) {
            first = false;
            result = part->evaluate(context);
          } else {
            if (auto ce = dynamic_cast<CallExpr*>(part.get())) {
              auto target = ce->object->evaluate(context);
              Value::CallableArgs vargs;
              vargs.push_back({"", result});
              for (const auto& arg : ce->args) {
                  vargs.push_back({arg.first, arg.second->evaluate(context)});
              }
              result = target.call(context, vargs);
            } else {
              auto callable = part->evaluate(context);
              result = callable.call(context, {{"", result}});
            }
          }
        }
        return result;
    }

    void prepend(std::unique_ptr<Expression> && e) {
        parts.insert(parts.begin(), std::move(e));
    }
};

std::string strip(const std::string & s) {
  static std::regex trailing_spaces_regex("^\\s+|\\s+$");
  return std::regex_replace(s, trailing_spaces_regex, "");
}

class JinjaParser {
private:
    using CharIterator = std::string::const_iterator;

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
    std::unique_ptr<Expression> parseExpression() {
        auto left = parseLogicalOr();
        if (it == end) return left;

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

        static std::regex compare_tok(R"(==|!=|<=?|>=?|in\b|is\b)");
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
            else throw std::runtime_error("Unknown comparison operator: " + op_str);
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }
        return left;
    }

    Expression::CallableArgs parseCallParams() {
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
                    result.emplace_back(ident->get_name(), std::move(value));
                } else {
                    result.emplace_back(std::string(), std::move(expr));
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
            auto filter = parseFilterExpression();
            filter->prepend(std::move(left));
            return filter;
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
                auto callParams = parseCallParams();
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
              auto callParams = parseCallParams();
              value = nonstd_make_unique<MethodCallExpr>(std::move(value), identifier, std::move(callParams));
            } else {
              value = nonstd_make_unique<SubscriptExpr>(std::move(value), nonstd_make_unique<LiteralExpr>(Value(identifier)));
            }
        }
        consumeSpaces();
      }

      if (peekSymbols({ "(" })) {
        auto callParams = parseCallParams();
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
      static std::regex block_keyword_tok(R"((if|else|elif|endif|for|endfor|set|block|endblock)\b)");
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
              auto iterable = parseExpression();
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

                if (consumeToken("=").empty()) throw std::runtime_error("Expected equals sign in set block");

                auto value = parseExpression();
                if (!value) throw std::runtime_error("Expected value in set block");

                auto post_space = parseBlockClose();
                tokens.push_back(nonstd_make_unique<SetTemplateToken>(pos, pre_space, post_space, varnames, std::move(value)));
              }
            } else if (keyword == "block") {
              auto blockname = parseIdentifier();
              if (blockname.empty()) throw std::runtime_error("Expected block name in block block");

              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<BlockTemplateToken>(pos, pre_space, post_space, blockname));
            } else if (keyword == "endblock") {
              auto post_space = parseBlockClose();
              tokens.push_back(nonstd_make_unique<EndBlockTemplateToken>(pos, pre_space, post_space));
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
              if (it == end || (*(it++))->type != TemplateToken::Type::EndFor) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<ForNode>(for_token->var_names, std::move(for_token->iterable), std::move(for_token->condition), std::move(body), for_token->recursive));
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
              children.emplace_back(nonstd_make_unique<VariableNode>(std::move(expr_token->expr), std::vector<std::string>()));
          } else if (auto set_token = dynamic_cast<SetTemplateToken*>(token.get())) {
              children.emplace_back(nonstd_make_unique<SetNode>(set_token->var_names, std::move(set_token->value)));
          } else if (auto namespaced_set_token = dynamic_cast<NamespacedSetTemplateToken*>(token.get())) {
              children.emplace_back(nonstd_make_unique<NamespacedSetNode>(namespaced_set_token->ns, namespaced_set_token->name, std::move(namespaced_set_token->value)));
          } else if (auto block_token = dynamic_cast<BlockTemplateToken*>(token.get())) {
              auto body = parseTemplate(begin, it, end);
              if (it == end || (*(it++))->type != TemplateToken::Type::EndBlock) {
                  throw unterminated(**start);
              }
              children.emplace_back(nonstd_make_unique<BlockNode>(block_token->name, std::move(body)));
          } else if (auto comment_token = dynamic_cast<CommentTemplateToken*>(token.get())) {
              // Ignore comments
          } else if (dynamic_cast<EndBlockTemplateToken*>(token.get())
                  || dynamic_cast<EndForTemplateToken*>(token.get())
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

    std::string template_str;
    CharIterator start, end, it;
      
    JinjaParser(const std::string& template_str) : template_str(template_str) {
      start = it = this->template_str.begin();
      end = this->template_str.end();
    }

public:

    static std::unique_ptr<TemplateNode> parse(const std::string& template_str) {
        JinjaParser parser(template_str);

        auto tokens = parser.tokenize();
        TemplateTokenIterator begin = tokens.begin();
        auto it = begin;
        TemplateTokenIterator end = tokens.end();
        return parser.parseTemplate(begin, it, end, /* full= */ true);
    }
};

Value simple_function(const std::string & fn_name, const std::vector<std::string> & params, const std::function<Value(const Value &, const Value & args)> & fn) {
  std::map<std::string, size_t> named_positions;
  for (size_t i = 0, n = params.size(); i < n; i++) named_positions[params[i]] = i;

  return Value::callable([=](const Value & context, const Value::CallableArgs & args) -> Value {
    auto args_obj = Value::object();
    auto positional = true;
    std::vector<bool> provided_args(params.size());
    for (size_t i = 0, n = args.size(); i < n; i++) {
      auto & arg = args[i];
      if (positional) {
        if (arg.first.empty() || (i < params.size() && arg.first == params[i])) {
          if (i >= params.size()) throw std::runtime_error("Too many positional params for " + fn_name);
          args_obj.set(params[i], arg.second);
          provided_args[i] = true;
          continue;
        } else {
          positional = false;
        }
      }
      auto named_pos_it = named_positions.find(arg.first);
      if (named_pos_it == named_positions.end()) {
        throw std::runtime_error("Unknown argument " + arg.first + " for function " + fn_name);
      }
      if (!positional) {
        provided_args[named_pos_it->second] = true;
      }
      args_obj.set(arg.first, arg.second);
    }
    // for (size_t i = 0, n = params.size(); i < n; i++) {
    //   if (!provided_args[i]) {
    //     throw std::runtime_error("Missing argument " + params[i] + " for function " + fn_name);
    //   }
    // }
    return fn(context, args_obj);
  });
}

Value Value::context(const Value & values) {
  auto context = Value::object();

  context.set("raise_exception", simple_function("raise_exception", { "message" }, [](const Value & context, const Value & args) -> Value {
    throw std::runtime_error(args.at("message").get<std::string>());
  }));
  context.set("tojson", simple_function("tojson", { "value", "indent" }, [](const Value & context, const Value & args) {
    return Value(args.at("value").dump(args.get<int64_t>("indent", -1)));
  }));
  context.set("trim", simple_function("trim", { "text" }, [](const Value & context, const Value & args) {
    return Value(strip(args.at("text").get<std::string>()));
  }));
  context.set("count", simple_function("count", { "items" }, [](const Value & context, const Value & args) {
    return Value((int64_t) args.at("items").size());
  }));
  context.set("join", simple_function("join", { "items", "d" }, [](const Value & context, const Value & args) {
    auto do_join = [](const Value & items, const std::string & sep) {
      std::ostringstream oss;
      auto first = true;
      for (size_t i = 0, n = items.size(); i < n; ++i) {
        if (first) first = false;
        else oss << sep;
        oss << items.at(i).get<std::string>();
      }
      return Value(oss.str());
    };
    auto sep = args.get<std::string>("d", "");
    if (args.contains("items")) {
        auto & items = args.at("items");
        return do_join(items, sep);
    } else {
      return simple_function("", {"items"}, [sep, do_join](const Value & context, const Value & args) {
        auto & items = args.at("items");
        if (!items || !items.is_array()) throw std::runtime_error("join expects an array for items, got: " + items.dump());
        return do_join(items, sep);
      });
    }
  }));
  context.set("namespace", callable([=](const Value & context, const Value::CallableArgs & args) {
    auto ns = Value::object();
    for (auto & arg : args) {
      ns.set(arg.first, arg.second);
    }
    return ns;
  }));
  context.set("equalto", simple_function("equalto", { "expected", "actual" }, [](const Value & context, const Value & args) {
      return Value(args.at("actual") == args.at("expected"));
  }));
  auto make_filter = [](const Value & filter, const Value & extra_args) -> Value {
    return simple_function("", { "value" }, [=](const Value & context, const Value & args) {
      auto value = args.at("value");
      Value::CallableArgs actual_args;
      actual_args.emplace_back("", value);
      for (size_t i = 0, n = extra_args.size(); i < n; i++) {
        actual_args.emplace_back("", extra_args.at(i));
      }
      return filter.call(context, actual_args);
    });
  };
  // https://jinja.palletsprojects.com/en/3.0.x/templates/#jinja-filters.reject
  context.set("reject", callable([=](const Value & context, const Value::CallableArgs & args) {
    auto & items = args[0].second;
    auto filter_fn = context.get(args[1].second);
    if (!filter_fn) throw std::runtime_error("Function not found " + args[1].second.dump());

    auto filter_args = Value::array();
    for (size_t i = 2, n = args.size(); i < n; i++) {
      filter_args.push_back(args[i].second);
    }
    auto filter = make_filter(filter_fn, filter_args);

    auto res = Value::array();
    for (size_t i = 0, n = items.size(); i < n; i++) {
      auto & item = items.at(i);
      auto pred_res = filter.call(context, { { "", item } });
      std::cerr << "Predicate result for " << item.dump() << ": " << (pred_res ? "true" : "false") << std::endl;
      if (!pred_res) {
        res.push_back(item);
      }
    }
    return res;
  }));
  context.set("range", callable([=](const Value & context, const Value::CallableArgs & args) {
    int64_t start = 0;
    int64_t end = 0;
    int64_t step = 1;
    if (args.size() == 1) {
      if (!args[0].first.empty()) throw std::runtime_error("When range is called with just 1 argument it must be positional");
      end = args[0].second.get<int64_t>();
    } else {
      auto positional = true;
      for (size_t i = 0; i < args.size(); i++) {
        auto & arg = args[i];
        auto v = arg.second.get<int64_t>();
        if (positional && i < 3 && arg.first.empty()) {
          (i == 0 ? start : i == 1 ? end : step) = v;
        } else {
          positional = false;
          if (arg.first == "start") start = v;
          else if (arg.first == "end") end = v;
          else if (arg.first == "step") step = v;
          else throw std::runtime_error("Unknown argument " + arg.first + " for function range");
        }
      }
    }
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

  if (values && !values.is_null()) {
    if (!values.is_object()) {
      throw std::runtime_error("Values must be an object");
    }
    auto keys = values.keys();
    for (size_t i = 0, n = keys.size(); i < n; i++) {
      auto & key = keys.at(i);
      context.set(key, values.at(key));
    }
  }
  return context;
}

}  // namespace jinja
