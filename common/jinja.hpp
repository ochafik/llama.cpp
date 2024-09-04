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

using json = nlohmann::json;

/*
* Backport nonstd_make_unique from C++14.
*
* NOTE: This code came up with the following stackoverflow post:
* https://stackoverflow.com/questions/10149840/c-arrays-and-make-unique
*
*/

template <class T, class... Args>
typename std::enable_if<!std::is_array<T>::value, std::unique_ptr<T>>::type
nonstd_make_unique(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

template <class T>
typename std::enable_if<std::is_array<T>::value, std::unique_ptr<T>>::type
nonstd_make_unique(std::size_t n) {
  typedef typename std::remove_extent<T>::type RT;
  return std::unique_ptr<T>(new RT[n]);
}

bool to_bool(const json& j) {
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number()) return j.get<double>() != 0;
  if (j.is_string()) return !j.get<std::string>().empty();
  if (j.is_array()) return !j.empty();
  // TODO: check truthfulness semantics of jinja
  return !j.is_null();
}


// Forward declarations
class TemplateNode;
class Expression;

enum SpaceHandling {
    Keep,
    Strip,
    KeepLines,
};

class TemplateToken {
public:
    enum class Type {
        Text,
        Variable,
        If,
        Else,
        Elif,
        EndIf,
        For,
        EndFor,
        Set,
        Comment,
        Block,
        EndBlock,
    };

    static std::string typeToString(Type t) {
        switch (t) {
            case Type::Text: return "Text";
            case Type::Variable: return "Variable";
            case Type::If: return "If";
            case Type::Else: return "Else";
            case Type::Elif: return "Elif";
            case Type::EndIf: return "EndIf";
            case Type::For: return "For";
            case Type::EndFor: return "EndFor";
            case Type::Set: return "Set";
            case Type::Comment: return "Comment";
            case Type::Block: return "Block";
            case Type::EndBlock: return "EndBlock";
        }
        return "Unknown";
    }

    std::runtime_error unexpected(const std::string & context) const {
      return std::runtime_error("Unexpected token in " + context + ": " + TemplateToken::typeToString(getType()));
    }
    std::runtime_error unterminated(const std::string & context) const {
      return std::runtime_error("Unterminated " + context + ": " + TemplateToken::typeToString(getType()));
    }
    void expectType(Type type) const {
      if (getType() != type) {
        throw unexpected("expecting " + typeToString(type));
      }
    }

    TemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : pos(pos), pre_space(pre), post_space(post) {}
    virtual ~TemplateToken() = default;
    virtual TemplateToken::Type getType() const = 0;

    size_t pos;
    SpaceHandling pre_space = SpaceHandling::Keep;
    SpaceHandling post_space = SpaceHandling::Keep;
};

struct TextTemplateToken : public TemplateToken {
    std::string text;
    TextTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& t) : TemplateToken(pos, pre, post), text(t) {}
    Type getType() const override { return Type::Text; }
};

struct VariableTemplateToken : public TemplateToken {
    std::unique_ptr<Expression> expr;
    VariableTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, std::unique_ptr<Expression> && e) : TemplateToken(pos, pre, post), expr(std::move(e)) {}
    Type getType() const override { return Type::Variable; }
};

struct IfTemplateToken : public TemplateToken {
    std::unique_ptr<Expression> condition;
    IfTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, std::unique_ptr<Expression> && c) : TemplateToken(pos, pre, post), condition(std::move(c)) {}
    Type getType() const override { return Type::If; }
};

struct ElifTemplateToken : public IfTemplateToken {
    ElifTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, std::unique_ptr<Expression> && c) : IfTemplateToken(pos, pre, post, std::move(c)) {}
    Type getType() const override { return Type::Elif; }
};

struct ElseTemplateToken : public TemplateToken {
  ElseTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(pos, pre, post) {}
    Type getType() const override { return Type::Else; }
};

struct EndIfTemplateToken : public TemplateToken {
  EndIfTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(pos, pre, post) {}
    Type getType() const override { return Type::EndIf; }
};

struct BlockTemplateToken : public TemplateToken {
    std::string name;
    BlockTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& n) : TemplateToken(pos, pre, post), name(n) {}
    Type getType() const override { return Type::Block; }
};

struct EndBlockTemplateToken : public TemplateToken {
  EndBlockTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(pos, pre, post) {}
    Type getType() const override { return Type::EndBlock; }
};

struct ForTemplateToken : public TemplateToken {
    std::vector<std::string> var_names;
    std::unique_ptr<Expression> iterable;
    std::unique_ptr<Expression> condition;
    bool recursive;
    ForTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::vector<std::string> & vns, std::unique_ptr<Expression> && iter,
      std::unique_ptr<Expression> && c, bool r)
      : TemplateToken(pos, pre, post), var_names(vns), condition(std::move(c)),
        iterable(std::move(iter)), recursive(r) {}
    Type getType() const override { return Type::For; }
};

struct EndForTemplateToken : public TemplateToken {
  EndForTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post) : TemplateToken(pos, pre, post) {}
    Type getType() const override { return Type::EndFor; }
};

struct SetTemplateToken : public TemplateToken {
    std::string var_name;
    std::unique_ptr<Expression> value;
    SetTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& vn, std::unique_ptr<Expression> && v)
      : TemplateToken(pos, pre, post), var_name(vn), value(std::move(v)) {}
    Type getType() const override { return Type::Set; }
};

struct CommentTemplateToken : public TemplateToken {
    std::string text;
    CommentTemplateToken(size_t pos, SpaceHandling pre, SpaceHandling post, const std::string& t) : TemplateToken(pos, pre, post), text(t) {}
    Type getType() const override { return Type::Comment; }
};

class TemplateNode {
public:

    enum class Type {
        Sequence,
        Text,
        Variable,
        NamedBlock,
        If,
        For,
        Set,
        Expression
    };
    virtual ~TemplateNode() = default;
    virtual void render(std::ostringstream& oss, json& context) const = 0;
    Type getType() const { return type; }

    std::string render(json & context) const {
        std::ostringstream oss;
        render(oss, context);
        return oss.str();
    }
private:
    Type type;
protected:
    TemplateNode(Type t) : type(t) {}
};

class SequenceNode : public TemplateNode {
    std::vector<std::unique_ptr<TemplateNode>> children;
public:
    SequenceNode(std::vector<std::unique_ptr<TemplateNode>> && c) : TemplateNode(Type::Sequence), children(std::move(c)) {}
    void render(std::ostringstream& oss, json& context) const override {
        for (const auto& child : children) {
            child->render(oss, context);
        }
    }
};

class TextNode : public TemplateNode {
    std::string text;
public:
    TextNode(const std::string& t) : TemplateNode(Type::Text), text(t) {}
    void render(std::ostringstream& oss, json&) const override { oss << text; }
};

class VariableNode : public TemplateNode {
    std::unique_ptr<Expression> expr;
public:
    VariableNode(std::unique_ptr<Expression> && e, std::vector<std::string> && f) 
        : TemplateNode(Type::Variable), expr(std::move(e)) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class IfNode : public TemplateNode {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> cascade;
public:
    IfNode(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> && c)
        : TemplateNode(Type::If), cascade(std::move(c)) {}
    void render(std::ostringstream& oss, json& context) const override;
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
            : TemplateNode(Type::For), var_names(vns), condition(std::move(c)),
            iterable(std::move(iter)), body(std::move(b)), recursive(r) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class BlockNode : public TemplateNode {
    std::string name;
    std::unique_ptr<TemplateNode> body;
public:
    BlockNode(const std::string& n, std::unique_ptr<TemplateNode> && b)
        : TemplateNode(Type::NamedBlock), name(n), body(std::move(b)) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class SetNode : public TemplateNode {
    std::string var_name;
    std::unique_ptr<Expression> value;
public:
    SetNode(const std::string& vn, std::unique_ptr<Expression> && v)
      : TemplateNode(Type::Set), var_name(vn), value(std::move(v)) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class Expression {
public:
    virtual ~Expression() = default;
    virtual json evaluate(json& context) const = 0;


    virtual json evaluateAsPipe(json& context, json& input) const {
      throw std::runtime_error("This expression cannot be used as a pipe");
    }
};

class IfExpr : public Expression {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> then_expr;
    std::unique_ptr<Expression> else_expr;
public:
    IfExpr(std::unique_ptr<Expression> && c, std::unique_ptr<Expression> && t, std::unique_ptr<Expression> && e)
        : condition(std::move(c)), then_expr(std::move(t)), else_expr(std::move(e)) {}
    json evaluate(json& context) const override {
        return to_bool(condition->evaluate(context)) ? then_expr->evaluate(context) : else_expr->evaluate(context);
    }
};

class LiteralExpr : public Expression {
    json value;
public:
    LiteralExpr(const json& v) : value(v) {}
    json evaluate(json&) const override { return value; }
};

class ArrayExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> elements;
public:
    ArrayExpr(std::vector<std::unique_ptr<Expression>> && e) : elements(std::move(e)) {}
    json evaluate(json & context) const override {
        json result = json::array();
        for (const auto& e : elements) {
            result.push_back(e->evaluate(context));
        }
        return result;
    }
};

class DictExpr : public Expression {
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> elements;
public:
    DictExpr(std::vector<std::pair<std::string, std::unique_ptr<Expression>>> && e) : elements(std::move(e)) {}
    json evaluate(json & context) const override {
        json result = json::object();
        for (const auto& e : elements) {
            result[e.first] = e.second->evaluate(context);
        }
        return result;
    }
};

class SubscriptExpr : public Expression {
    std::unique_ptr<Expression> base;
    std::unique_ptr<Expression> index;
public:
    SubscriptExpr(std::unique_ptr<Expression> && b, std::unique_ptr<Expression> && i)
        : base(std::move(b)), index(std::move(i)) {}
    json evaluate(json & context) const override {
        json target = base->evaluate(context);
        if (target.is_array()) {
            return target.at(index->evaluate(context).get<int>());
        } else if (target.is_object()) {
            return target.at(index->evaluate(context).get<std::string>());
        } else {
            throw std::runtime_error("Subscripting non-array or non-object");
        }
    }
};

class VariableExpr : public Expression {
    std::string name;
public:
    VariableExpr(const std::string& n) : name(n) {}
    std::string get_name() const { return name; }
    json evaluate(json& context) const override {
        return context.value(name, json(nullptr));
    }
};

json operator+(const json& lhs, const json& rhs) {
    if (lhs.is_number_integer() && rhs.is_number_integer())
      return lhs.get<int64_t>() + rhs.get<int64_t>();
    else
      return lhs.get<double>() + rhs.get<double>();
}

json operator-(const json& lhs) {
    if (lhs.is_number_integer())
      return -lhs.get<int64_t>();
    else
      return -lhs.get<double>();
}

json operator-(const json& lhs, const json& rhs) {
    if (lhs.is_number_integer() && rhs.is_number_integer())
      return lhs.get<int64_t>() - rhs.get<int64_t>();
    else
      return lhs.get<double>() - rhs.get<double>();
}

json operator*(const json& lhs, const json& rhs) {
    if (lhs.is_number_integer() && rhs.is_number_integer())
      return lhs.get<int64_t>() * rhs.get<int64_t>();
    else
      return lhs.get<double>() * rhs.get<double>();
}

json operator/(const json& lhs, const json& rhs) {
    if (lhs.is_number_integer() && rhs.is_number_integer())
      return lhs.get<int64_t>() / rhs.get<int64_t>();
    else
      return lhs.get<double>() / rhs.get<double>();
}

json operator%(const json& lhs, const json& rhs) {
  return lhs.get<int64_t>() % rhs.get<int64_t>();
}

class UnaryOpExpr : public Expression {
public:
    enum class Op { Plus, Minus, LogicalNot };
private:
    std::unique_ptr<Expression> expr;
    Op op;
public:
    UnaryOpExpr(std::unique_ptr<Expression> && e, Op o) : expr(std::move(e)), op(o) {}
    json evaluate(json& context) const override {
        json e = expr->evaluate(context);
        switch (op) {
            case Op::Plus: return e;
            case Op::Minus: return -e;
            case Op::LogicalNot: return !to_bool(e);
        }
        throw std::runtime_error("Unknown unary operator");
    }
};

class BinaryOpExpr : public Expression {
public:
    enum class Op { StrConcat, Add, Sub, Mul, MulMul, Div, DivDiv, Mod, Eq, Ne, Lt, Gt, Le, Ge, And, Or, In, Is };
private:
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    Op op;
public:
    BinaryOpExpr(std::unique_ptr<Expression> && l, std::unique_ptr<Expression> && r, Op o)
        : left(std::move(l)), right(std::move(r)), op(o) {}
    json evaluate(json& context) const override {
        json l = left->evaluate(context);
        
        if (op == Op::Is) {
          auto t = dynamic_cast<VariableExpr*>(right.get());
          if (!t) throw std::runtime_error("Right side of 'is' operator must be a variable");

          const auto & name = t->get_name();
          if (name == "boolean") return l.is_boolean();
          if (name == "integer") return l.is_number_integer();
          if (name == "float") return l.is_number_float();
          if (name == "number") return l.is_number();
          if (name == "string") return l.is_string();
          if (name == "mapping") return l.is_object();
          if (name == "iterable") return l.is_array();
          if (name == "sequence") return l.is_array();
          throw std::runtime_error("Unknown type for 'is' operator: " + name);
        }

        json r = right->evaluate(context);
        switch (op) {
            case Op::StrConcat: return l.get<std::string>() + r.get<std::string>();
            case Op::Add:       return l + r;
            case Op::Sub:       return l - r;
            case Op::Mul:       return l * r;
            case Op::MulMul:    return std::pow(l.get<double>(), r.get<double>());
            case Op::Div:       return l / r;
            case Op::DivDiv:    return l.get<int>() / r.get<int>();
            case Op::Mod:       return l.get<int>() % r.get<int>();
            case Op::Eq:        return l == r;
            case Op::Ne:        return l != r;
            case Op::Lt:        return l < r;
            case Op::Gt:        return l > r;
            case Op::Le:        return l <= r;
            case Op::Ge:        return l >= r;
            case Op::And:       return to_bool(l) && to_bool(r);
            case Op::Or:        return to_bool(l) || to_bool(r);
            case Op::In:        return r.is_array() && r.find(l) != r.end();
            default:            break;
        }
        throw std::runtime_error("Unknown binary operator");
    }
};

class MethodCallExpr : public Expression {
    std::unique_ptr<Expression> object; // If nullptr, this is a function call
    std::string method;
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> args;
public:
    MethodCallExpr(std::unique_ptr<Expression> && obj, const std::string& m, std::vector<std::pair<std::string, std::unique_ptr<Expression>>> && a)
        : object(std::move(obj)), method(m), args(std::move(a)) {}
    bool is_function_call() const { return object == nullptr; }
    const std::string& get_method() const { return method; }
    // const std::vector<std::unique_ptr<Expression>>& get_args() const { return args; }
    json evaluate(json& context) const override {
        json obj = object->evaluate(context);
        if (method == "append" && obj.is_array()) {
            if (args.size() != 1 || !args[0].first.empty()) throw std::runtime_error("append method must have exactly one unnamed argument");
            obj.push_back(args[0].second->evaluate(context));
            // for (const auto& arg : args) {
            //     obj.push_back(arg.second->evaluate(context));
            // }
            return json();
        }
        throw std::runtime_error("Unknown method: " + method);
    }
};

class FunctionCallExpr : public Expression {
    std::string name;
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> args;

    json getSingleArg(const std::string & arg_name, bool allowPositional, json& context, bool has_default_value = false, const json & default_value = json()) const {
      if (args.empty()) {
        if (has_default_value) return default_value;
        throw std::runtime_error("Function " + name + " must have exactly one argument (" + arg_name + ")");
      }
      if (args.size() != 1) throw std::runtime_error("Function " + name + " must have exactly one argument (" + arg_name + ")");
      if (!args[0].first.empty()) {
        if (args[0].first != arg_name) throw std::runtime_error("Function " + name + " argument name mismatch: " + args[0].first + " != " + arg_name);
      } else {
        if (!allowPositional) throw std::runtime_error("Function " + name + " argument " + arg_name + " must be provided by name, not position");
      }
      return args[0].second->evaluate(context);
    }
    
public:
    FunctionCallExpr(const std::string& n, std::vector<std::pair<std::string, std::unique_ptr<Expression>>> && a = {})
        : name(n), args(std::move(a)) {}
    
    json evaluate(json& context) const override {
        throw std::runtime_error("Unknown function (or maybe can only be evaluated as a pipe): " + name);
    }
    
    json evaluateAsPipe(json& context, json& input) const override {
        json result;
        if (name == "tojson") {
          auto indent = getSingleArg("indent", true, context, 0);
          result = result.dump(indent);
        } else if (name == "join") {
          auto sep = getSingleArg("d", true, context, "").get<std::string>();
          std::ostringstream oss;
          auto first = true;
          for (const auto& item : result) {
            if (first) first = false;
            else oss << sep;
            oss << item.get<std::string>();
          }
          result = oss.str();
        } else {
          throw std::runtime_error("Unknown pipe function: " + name);
        }
        return result;
    }
};

class FilterExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> parts;
public:
    FilterExpr(std::vector<std::unique_ptr<Expression>> && p) : parts(std::move(p)) {}
    json evaluate(json& context) const override {
        json result;
        bool first = true;
        for (const auto& part : parts) {
          if (first) {
            first = false;
            result = part->evaluate(context);
          } else {
            result = part->evaluateAsPipe(context, result);
          }
        }
        return result;
    }

    void prepend(std::unique_ptr<Expression> && e) {
        parts.insert(parts.begin(), std::move(e));
    }
};

void VariableNode::render(std::ostringstream& oss, json& context) const {
    json result = expr->evaluate(context);
    if (result.is_string()) {
        oss << result.get<std::string>();
    } else {
        // TODO: what is the behaviour here, should we explode when given an object? Be silent with null?
        oss << result.dump();
    }
}

void IfNode::render(std::ostringstream& oss, json& context) const {
    for (const auto& branch : cascade) {
        if (to_bool(branch.first->evaluate(context))) {
            branch.second->render(oss, context);
            return;
        }
    }
}

void ForNode::render(std::ostringstream& oss, json& context) const {
    json iterable_value = iterable->evaluate(context);
    if (!iterable_value.is_array()) {
      throw std::runtime_error("For loop iterable must be iterable");
    }

    // json original_context = context;
    json original_vars = json::object();
    for (const auto& var_name : var_names) {
        original_vars[var_name] = context.contains(var_name) ? context[var_name] : json();
    }

    auto loop_iteration = [&](const json& item) {
        if (var_names.size() == 1) {
            context[var_names[0]] = item;
        } else {
            if (!item.is_array() || item.size() != var_names.size()) {
                throw std::runtime_error("Mismatched number of variables and items in for loop");
            }
            for (size_t i = 0; i < var_names.size(); ++i) {
                context[var_names[i]] = item[i];
            }
        }
        if (!condition || to_bool(condition->evaluate(context))) {
          body->render(oss, context);
        }
    };
    std::function<void(const json&)> visit = [&](const json& iter) {
        for (const auto& item : iter) {
            if (item.is_array() && recursive) {
                visit(item);
            // } else if (item.is_object()) {
            //     visit(item, recursive);
            } else {
                loop_iteration(item);
            }
        }
    };
    visit(iterable_value);
    
    for (const auto & pair : original_vars.items()) {
        if (pair.value().is_null()) {
            context.erase(pair.key());
        } else {
            context[pair.key()] = pair.value();
        }
    }
}

void BlockNode::render(std::ostringstream& oss, json& context) const {
    body->render(oss, context);
}

void SetNode::render(std::ostringstream&, json& context) const {
    context[var_name] = value->evaluate(context);
}


class JinjaParser {
private:
    using CharIterator = std::string::const_iterator;

    // std::vector<std::unique_ptr<TemplateNode>> ast;

    void consumeSpaces(CharIterator & it, const CharIterator & end) const {
        while (it != end && std::isspace(*it)) ++it;
    }
    std::unique_ptr<std::string> parseString(CharIterator & it, const CharIterator & end) const {

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
              // case quote: result += '"'; break;
              // default: result += *it; break;
            }
          } else if (*it == '\\') {
            escape = true;
          } else if (*it == quote) {
              ++it; // Move past the closing quote
            return nonstd_make_unique<std::string>(result);
          } else {
            result += *it;
          }
        }
        return nullptr;
      };

      consumeSpaces(it, end);
      if (it == end) return nullptr;
      if (*it == '"') return doParse('"');
      if (*it == '\'') return doParse('\'');
      return nullptr;
    }

    json parseNumber(CharIterator& it, const CharIterator& end) const {
        consumeSpaces(it, end);
        auto start = it;
        bool hasDecimal = false;
        bool hasExponent = false;

        if (it != end && (*it == '-' || *it == '+')) ++it;

        while (it != end && (std::isdigit(*it) || *it == '.' || *it == 'e' || *it == 'E' || *it == '-' || *it == '+')) {
            if (*it == '.') {
                if (hasDecimal) return json(); // Multiple decimal points
                hasDecimal = true;
            } else if (*it == 'e' || *it == 'E') {
                if (hasExponent) return json(); // Multiple exponents
                hasExponent = true;
            }
            ++it;
        }

        if (start == it) return json(); // No valid characters found

        std::string str(start, it);
        try {
          return std::stoll(str);
        } catch (...) {
          try {
            return std::stod(str);
          } catch (...) {
              return json(); // Invalid number format
          }
        }
    }

    /** integer, float, bool, string */
    json parseConstant(CharIterator & it, const CharIterator & end) const {
      consumeSpaces(it, end);
      if (it == end) return nullptr;
      if (*it == '"' || *it == '\'') {
        auto str = parseString(it, end);
        if (str) return *str;
      }
      if (*it == 't') {
        if (std::distance(it, end) >= 4 && std::string(it, it + 4) == "true") {
          it += 4;
          return true;
        }
      }
      if (*it == 'f') {
        if (std::distance(it, end) >= 5 && std::string(it, it + 5) == "false") {
          it += 5;
          return false;
        }
      }
      if (*it == 'n') {
        if (std::distance(it, end) >= 4 && std::string(it, it + 4) == "null") {
          it += 4;
          return nullptr;
        }
      }

      auto number = parseNumber(it, end);
      return number;
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
    

    expression_parsing_error expr_parse_error(const std::string & message, const CharIterator & it, const CharIterator & end) const {
        return expression_parsing_error(message, it);
    }

    bool peekSymbols(const std::vector<std::string> & symbols, const CharIterator & it, const CharIterator & end) const {
        for (const auto & symbol : symbols) {
            if (std::distance(it, end) >= symbol.size() && std::string(it, it + symbol.size()) == symbol) {
                return true;
            }
        }
        return false;
    }

    std::string consumeToken(const std::regex & regex, CharIterator & it, const CharIterator & end) const {
        consumeSpaces(it, end);
        std::smatch match;
        if (std::regex_search(it, end, match, regex) && match.position() == 0) {
            it += match[0].length();
            return match[0].str();
        }
        return "";
    }

    std::string consumeToken(const std::string & token, CharIterator & it, const CharIterator & end) const {
        consumeSpaces(it, end);
        if (std::distance(it, end) >= token.size() && std::string(it, it + token.size()) == token) {
            it += token.size();
            return token;
        }
        return "";
    }

    /**
      * - FullExpression = LogicalOr ("if" IfExpression)?
      * - IfExpression = LogicalOr "else" FullExpression
      * - LogicalOr = LogicalAnd ("or" LogicalOr)? = LogicalAnd ("or" LogicalAnd)*
      * - LogicalAnd = LogicalCompare ("and" LogicalAnd)? = LogicalCompare ("and" LogicalCompare)*
      * - LogicalCompare = StringConcat ((("==" | "!=" | "<" | ">" | "<=" | ">=" | "in") StringConcat) | "is" identifier CallParams)?
      * - StringConcat = MathPow ("~" LogicalAnd)?
      * - MathPow = MathPlusMinus ("**" MathPow)? = MatPlusMinus ("**" MathPlusMinus)*
      * - MathPlusMinus = MathMulDiv (("+" | "-") MathPlusMinus)? = MathMulDiv (("+" | "-") MathMulDiv)*
      * - MathMulDiv = MathUnaryPlusMinus (("*" | "/" | "//" | "%") MathMulDiv)? = MathUnaryPlusMinus (("*" | "/" | "//" | "%") MathUnaryPlusMinus)*
      * - MathUnaryPlusMinus = ("+" | "-" | "!")? ValueExpression ("|" FilterExpression)?
      * - FilterExpression = identifier CallParams ("|" FilterExpression)? = identifier CallParams ("|" identifier CallParams)*
      * - ValueExpression = (identifier | number | string | bool | BracedExpressionOrArray | Tuple | Dictionary ) SubScript? Call?
      * - BracedExpressionOrArray = "(" FullExpression ("," FullExpression)* ")"
      * - Tuple = "[" (FullExpression ("," FullExpression)*)? "]"
      * - Dictionary = "{" (string "=" FullExpression ("," string "=" FullExpression)*)? "}"
      * - SubScript = ("[" FullExpression "]" | "." identifier )+
      * - Call = CallParams 
      * - CallParams = "(" ((identifier "=")? FullExpression ("," (identifier "=")? FullExpression)*)? ")"
      */
    std::unique_ptr<Expression> parseExpression(const std::string & expr) const {
        auto it = expr.begin();
        auto end = expr.end();
        try {
          auto res = parseFullExpression(it, end);
          if (it != end) throw expr_parse_error("Unexpected characters at the end of the expression", it, end);
          return res;
        } catch (const expression_parsing_error & e) {
          std::string message = e.what();
          auto pos = e.get_pos(expr.begin());
          auto line = std::count(expr.begin(), expr.begin() + pos, '\n') + 1;
          auto col = pos - std::string(expr.begin(), expr.begin() + pos).rfind('\n');
          message += " at row " + std::to_string(line) + ", column " + std::to_string(col) + ": " + std::string(expr.begin() + pos, expr.end());
          throw std::runtime_error(message);
        }
    }
    std::unique_ptr<Expression> parseFullExpression(CharIterator & it, const CharIterator & end) const {
        auto left = parseLogicalOr(it, end);
        if (it == end) return left;

        static std::regex if_tok(R"(if\b)");
        if (consumeToken(if_tok, it, end).empty()) {
          return left;
        }

        auto if_expr = parseIfExpression(it, end);
        return nonstd_make_unique<IfExpr>(std::move(left), std::move(if_expr.first), std::move(if_expr.second));
        // throw std::runtime_error("Expected more after 'if' keyword");
    }

    std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>> parseIfExpression(CharIterator & it, const CharIterator & end) const {
        auto condition = parseLogicalOr(it, end);
        if (!condition) throw expr_parse_error("Expected condition expression", it, end);

        static std::regex else_tok(R"(else\b)");
        if (consumeToken(else_tok, it, end).empty()) throw std::runtime_error("Expected 'else' keyword");
        
        auto else_expr = parseFullExpression(it, end);
        if (!else_expr) throw expr_parse_error("Expected 'else' expression", it, end);

        return std::make_pair(std::move(condition), std::move(else_expr));
    }

    std::unique_ptr<Expression> parseLogicalOr(CharIterator & it, const CharIterator & end) const {
        auto left = parseLogicalAnd(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'logical or' expression", it, end);

        static std::regex or_tok(R"(or\b)");
        while (!consumeToken(or_tok, it, end).empty()) {
            auto right = parseLogicalAnd(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'or' expression", it, end);
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::Or);
        }
        return left;
    }

    std::unique_ptr<Expression> parseLogicalAnd(CharIterator & it, const CharIterator & end) const {
        auto left = parseLogicalCompare(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'logical and' expression", it, end);

        static std::regex and_tok(R"(and\b)");
        while (!consumeToken(and_tok, it, end).empty()) {
            auto right = parseLogicalCompare(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'and' expression", it, end);
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::And);
        }
        return left;
    }

    std::unique_ptr<Expression> parseLogicalCompare(CharIterator & it, const CharIterator & end) const {
        auto left = parseStringConcat(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'logical compare' expression", it, end);

        static std::regex compare_tok(R"((==|!=|<=?|>=?|in|is)\b)");
        std::smatch match;
        while (!consumeToken(compare_tok, it, end).empty()) {
            auto right = parseStringConcat(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'logical compare' expression", it, end);
            BinaryOpExpr::Op op;
            if (match[0] == "is") {
              auto identifier = parseIdentifier(it, end);
              auto callParams = parseCallParams(it, end);
              // if (!callParams) throw expr_parse_error("Expected call params after 'is' keyword", it, end);

              return nonstd_make_unique<BinaryOpExpr>(
                  std::move(left),
                  nonstd_make_unique<MethodCallExpr>(nullptr, identifier, std::move(callParams)),
                  BinaryOpExpr::Op::Is);
            }
            if (match[0] == "==") op = BinaryOpExpr::Op::Eq;
            else if (match[0] == "!=") op = BinaryOpExpr::Op::Ne;
            else if (match[0] == "<") op = BinaryOpExpr::Op::Lt;
            else if (match[0] == ">") op = BinaryOpExpr::Op::Gt;
            else if (match[0] == "<=") op = BinaryOpExpr::Op::Le;
            else if (match[0] == ">=") op = BinaryOpExpr::Op::Ge;
            else if (match[0] == "in") op = BinaryOpExpr::Op::In;
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }
        return left;
    }

    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> parseCallParams(CharIterator & it, const CharIterator & end) const {
        consumeSpaces(it, end);
        if (consumeToken("(", it, end).empty()) throw expr_parse_error("Expected opening parenthesis in call args", it, end);

        std::vector<std::pair<std::string, std::unique_ptr<Expression>>> result;
        
        while (it != end) {
            consumeSpaces(it, end);
            if (!consumeToken(")", it, end).empty()) {
                return result;
            }
            auto identifier = parseIdentifier(it, end);
            if (!identifier.empty()) {
                consumeSpaces(it, end);
                static std::regex single_eq_tok(R"(=(?!=))"); // negative lookahead to avoid consuming '=='
                if (!consumeToken(single_eq_tok, it, end).empty()) {
                    auto expr = parseFullExpression(it, end);
                    if (!expr) throw expr_parse_error("Expected expression in for named arg", it, end);
                    result.emplace_back(identifier, std::move(expr));
                } else {
                    result.emplace_back(std::string(), nonstd_make_unique<VariableExpr>(identifier));
                }
            } else {
                auto expr = parseFullExpression(it, end);
                if (!expr) throw expr_parse_error("Expected expression in call args", it, end);
                result.emplace_back(std::string(), std::move(expr));
            }
        }
        throw expr_parse_error("Expected closing parenthesis in call args", it, end);
    }

    std::string parseIdentifier(CharIterator & it, const CharIterator & end) const {
        static std::regex ident_regex(R"(\w+)");
        return consumeToken(ident_regex, it, end);
    }

    std::unique_ptr<Expression> parseStringConcat(CharIterator & it, const CharIterator & end) const {
        auto left = parseMathPow(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'string concat' expression", it, end);

        if (!consumeToken("~", it, end).empty()) {
            auto right = parseLogicalAnd(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'string concat' expression", it, end);
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::StrConcat);
        }
        return left;
    }

    std::unique_ptr<Expression> parseMathPow(CharIterator & it, const CharIterator & end) const {
        auto left = parseMathPlusMinus(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'math pow' expression", it, end);

        std::smatch match;
        while (!consumeToken("**", it, end).empty()) {
            auto right = parseMathPlusMinus(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'math pow' expression", it, end);
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), BinaryOpExpr::Op::MulMul);
        }
        return left;
    }

    std::unique_ptr<Expression> parseMathPlusMinus(CharIterator & it, const CharIterator & end) const {
        auto left = parseMathMulDiv(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'math plus/minus' expression", it, end);

        static std::regex plus_minus_tok(R"([-+])");
        std::string op_str;
        while (!(op_str = consumeToken(plus_minus_tok, it, end)).empty()) {
            auto right = parseMathMulDiv(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'math plus/minus' expression", it, end);
            auto op = op_str == "+" ? BinaryOpExpr::Op::Add : BinaryOpExpr::Op::Sub;
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }
        return left;
    }
    
    std::unique_ptr<Expression> parseMathMulDiv(CharIterator & it, const CharIterator & end) const {
        auto left = parseMathUnaryPlusMinus(it, end);
        if (!left) throw expr_parse_error("Expected left side of 'math mul/div' expression", it, end);

        static std::regex mul_div_tok(R"(\*\*?|//?|%)");
        std::string op_str;
        while (!(op_str = consumeToken(mul_div_tok, it, end)).empty()) {
            auto right = parseMathUnaryPlusMinus(it, end);
            if (!right) throw expr_parse_error("Expected right side of 'math mul/div' expression", it, end);
            auto op = op_str == "*" ? BinaryOpExpr::Op::Mul 
                : op_str == "**" ? BinaryOpExpr::Op::MulMul 
                : op_str == "/" ? BinaryOpExpr::Op::Div 
                : op_str == "//" ? BinaryOpExpr::Op::DivDiv
                : BinaryOpExpr::Op::Mod;
            left = nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        }

        if (!consumeToken("|", it, end).empty()) {
            auto filter = parseFilterExpression(it, end);
            filter->prepend(std::move(left));
            return filter;
        }
        return left;
    }

    std::unique_ptr<FilterExpr> parseFilterExpression(CharIterator & it, const CharIterator & end) const {
        std::vector<std::unique_ptr<Expression>> parts;
        auto parseFunctionCall = [&]() {
            auto identifier = parseIdentifier(it, end);
            if (identifier.empty()) throw expr_parse_error("Expected identifier in filter expression", it, end);

            if (peekSymbols({ "(" }, it, end)) {
                auto callParams = parseCallParams(it, end);
                parts.push_back(nonstd_make_unique<FunctionCallExpr>(identifier, std::move(callParams)));
            } else {
                parts.push_back(nonstd_make_unique<FunctionCallExpr>(identifier));
            }
        };
        parseFunctionCall();
        while (it != end && !consumeToken("|", it, end).empty()) {
            parseFunctionCall();
        }
        return nonstd_make_unique<FilterExpr>(std::move(parts));
    }

    std::unique_ptr<Expression> parseMathUnaryPlusMinus(CharIterator & it, const CharIterator & end) const {
        consumeSpaces(it, end);
        static std::regex unary_plus_minus_tok(R"([-+!])");
        auto op_str = consumeToken(unary_plus_minus_tok, it, end);
        auto expr = parseValueExpression(it, end);
        if (!expr) throw expr_parse_error("Expected expr of 'unary plus/minus' expression", it, end);
        
        if (!op_str.empty()) {
            auto op = op_str == "+" ? UnaryOpExpr::Op::Plus : op_str == "-" ? UnaryOpExpr::Op::Minus : UnaryOpExpr::Op::LogicalNot;
            return nonstd_make_unique<UnaryOpExpr>(std::move(expr), op);
        }
        return expr;
    }

    // * - ValueExpression = (identifier | number | string | bool | BracedExpressionOrArray | Tuple | Dictionary ) SubScript? Call?
    //   * - SubScript = ("[" FullExpression "]" | "." identifier )+
        
    std::unique_ptr<Expression> parseValueExpression(CharIterator & it, const CharIterator & end) const {
      auto parseValue = [&]() -> std::unique_ptr<Expression> {
        auto constant = parseConstant(it, end);
        if (!constant.is_null()) return nonstd_make_unique<LiteralExpr>(constant);

        static std::regex null_regex(R"(null\b)");
        if (!consumeToken(null_regex, it, end).empty()) return nonstd_make_unique<LiteralExpr>(json());

        auto identifier = parseIdentifier(it, end);
        if (!identifier.empty()) return nonstd_make_unique<VariableExpr>(identifier);

        auto braced = parseBracedExpressionOrArray(it, end);
        if (braced) return braced;

        auto array = parseArray(it, end);
        if (array) return array;

        auto dictionary = parseDictionary(it, end);
        if (dictionary) return dictionary;

        throw expr_parse_error("Expected value expression", it, end);
      };

      auto value = parseValue();
      
      while (it != end && peekSymbols({ "[", "." }, it, end)) {
        if (!consumeToken("[", it, end).empty()) {
            auto index = parseFullExpression(it, end);
            if (!index) throw expr_parse_error("Expected index in subscript", it, end);
            if (consumeToken("]", it, end).empty()) throw expr_parse_error("Expected closing bracket in subscript", it, end);
            
            value = nonstd_make_unique<SubscriptExpr>(std::move(value), std::move(index));
        } else if (!consumeToken(".", it, end).empty()) {
            auto identifier = parseIdentifier(it, end);
            if (identifier.empty()) throw expr_parse_error("Expected identifier in subscript", it, end);
            value = nonstd_make_unique<SubscriptExpr>(std::move(value), nonstd_make_unique<LiteralExpr>(identifier));
        }
      }
      return value;
    }

    std::unique_ptr<Expression> parseBracedExpressionOrArray(CharIterator & it, const CharIterator & end) const {
        if (consumeToken("(", it, end).empty()) return nullptr;
        
        auto expr = parseFullExpression(it, end);
        if (!expr) throw expr_parse_error("Expected expression in braced expression", it, end);
        
        if (!consumeToken(")", it, end).empty()) {
            // Drop the parentheses
            return expr;
        }

        std::vector<std::unique_ptr<Expression>> tuple;
        tuple.emplace_back(std::move(expr));

        while (it != end) {
          if (consumeToken(",", it, end).empty()) throw std::runtime_error("Expected comma in tuple");
          auto next = parseFullExpression(it, end);
          if (!next) throw std::runtime_error("Expected expression in tuple");
          tuple.push_back(std::move(next));

          if (!consumeToken(")", it, end).empty()) {
              return nonstd_make_unique<ArrayExpr>(std::move(tuple));
          }
        }
        throw std::runtime_error("Expected closing parenthesis");
    }

    std::unique_ptr<Expression> parseArray(CharIterator & it, const CharIterator & end) const {
        if (consumeToken("[", it, end).empty()) return nullptr;
        
        std::vector<std::unique_ptr<Expression>> elements;
        if (!consumeToken("]", it, end).empty()) {
            return nonstd_make_unique<ArrayExpr>(std::move(elements));
        }
        auto first_expr = parseFullExpression(it, end);
        if (!first_expr) throw std::runtime_error("Expected first expression in array");
        elements.push_back(std::move(first_expr));

        while (it != end) {
            if (!consumeToken(",", it, end).empty()) {
              auto expr = parseFullExpression(it, end);
              if (!expr) throw std::runtime_error("Expected expression in array");
              elements.push_back(std::move(expr));
            } else if (!consumeToken("]", it, end).empty()) {
                return nonstd_make_unique<ArrayExpr>(std::move(elements));
            } else {
                throw std::runtime_error("Expected comma or closing bracket in array");
            }
        }
        throw std::runtime_error("Expected closing bracket");
    }

    std::unique_ptr<Expression> parseDictionary(CharIterator & it, const CharIterator & end) const {
        if (consumeToken("{", it, end).empty()) return nullptr;
        
        std::vector<std::pair<std::string, std::unique_ptr<Expression>>> elements;
        if (!consumeToken("}", it, end).empty()) {
            return nonstd_make_unique<DictExpr>(std::move(elements));
        }

        auto parseKeyValuePair = [&]() {
            auto key = parseString(it, end);
            if (!key) throw std::runtime_error("Expected key in dictionary");
            if (consumeToken("=", it, end).empty()) throw std::runtime_error("Expected equals sign in dictionary");
            auto value = parseFullExpression(it, end);
            if (!value) throw std::runtime_error("Expected value in dictionary");
            elements.emplace_back(std::make_pair(*key, std::move(value)));
        };

        parseKeyValuePair();
        
        while (it != end) {
            if (!consumeToken(",", it, end).empty()) {
                parseKeyValuePair();
            } else if (!consumeToken("}", it, end).empty()) {
                return nonstd_make_unique<DictExpr>(std::move(elements));
            } else {
                throw std::runtime_error("Expected comma or closing brace in dictionary");
            }
        }
        throw std::runtime_error("Expected closing brace");
    }

    static SpaceHandling parseSpaceHandling(const std::string& s) {
        if (s == "-") return SpaceHandling::Strip;
        if (s == "~") return SpaceHandling::KeepLines;
        return SpaceHandling::Keep;
    }

    using TemplateTokenVector = std::vector<std::unique_ptr<TemplateToken>>;
    using TemplateTokenIterator = TemplateTokenVector::const_iterator;

    TemplateTokenVector tokenize() const {
      std::regex token_regex(R"(\{\{([-~]?)\s*(.*?)\s*([-~]?)\}\}|\{%([-~]?)\s*(.*?)\s*([-~]?)%\}|\{#\s*(.*?)\s*#\})");

      std::regex var_regex(R"(\{\{\s*(.*?)\s*\}\})");
      std::regex if_regex(R"((el)?if\b\s*(.*?))");
      std::regex for_regex(R"(for\s+((?:\w+)(?:\s*,\s*?:\w+)*)\s+in\b\s*(.*?)(\bif\b(.*?))?(?:\s*\b(recursive))?)");
      std::regex set_regex(R"(set\s+(\w+)\s*=\s*(.*?))");
      std::regex named_block_regex(R"(block\s+(\w+))");

      std::vector<std::unique_ptr<TemplateToken>> tokens;
      const auto start = template_str.begin();
      auto it = start;
      const auto end = template_str.end();
      std::smatch match;

      while (it != end) {
        auto pos = std::distance(start, it);
        SpaceHandling pre_space = SpaceHandling::Keep;
        SpaceHandling post_space = SpaceHandling::Keep;
        if (std::regex_search(it, end, match, token_regex)) {
          if (match.position() > 0) {
            tokens.push_back(nonstd_make_unique<TextTemplateToken>(pos, pre_space, post_space, std::string(it, it + match.position())));
          }
          it += match.position() + match.length();

          if (match[7].matched) {
            tokens.push_back(nonstd_make_unique<CommentTemplateToken>(pos, pre_space, post_space, match[7].str()));
            continue;
          }
          
          std::string content;
          bool is_block = false;
          if (match[1].matched) {
            pre_space = parseSpaceHandling(match[1].str());
            content = match[2].str();
            post_space = parseSpaceHandling(match[3].str());
          } else {
            pre_space = parseSpaceHandling(match[4].str());
            content = match[5].str();
            post_space = parseSpaceHandling(match[6].str());
            is_block = true;
          }
          
          if (is_block) {
            if (std::regex_match(content, match, set_regex)) {
                std::string var_name = match[1].str();
                auto value = parseExpression(match[2].str());
                tokens.push_back(nonstd_make_unique<SetTemplateToken>(pos, pre_space, post_space, var_name, std::move(value)));
            } else if (std::regex_match(content, match, if_regex)) {
                auto is_elif = match[1].matched;
                auto condition = parseExpression(match[2].str());
                tokens.push_back(
                  is_elif ? nonstd_make_unique<ElifTemplateToken>(pos, pre_space, post_space, std::move(condition))
                          : nonstd_make_unique<IfTemplateToken>(pos, pre_space, post_space, std::move(condition)));
            } else if (std::regex_match(content, match, for_regex)) {
                std::vector<std::string> var_names;
                std::istringstream iss(match[1].str());
                std::string var_name;
                while (std::getline(iss, var_name, ',')) {
                    var_names.push_back(std::regex_replace(var_name, std::regex("^\\s+|\\s+$"), ""));
                }
                auto iterable = parseExpression(match[2].str());
                std::unique_ptr<Expression> condition;
                if (match[3].matched) {
                    condition = parseExpression(match[4].str());
                }
                bool recursive = match[5].matched;
                tokens.push_back(nonstd_make_unique<ForTemplateToken>(pos, pre_space, post_space, std::move(var_names), std::move(iterable), std::move(condition), recursive));
            } else if (std::regex_match(content, match, named_block_regex)) {
                tokens.push_back(nonstd_make_unique<BlockTemplateToken>(pos, pre_space, post_space, match[1].str()));
            } else if (content == "else") {
                tokens.push_back(nonstd_make_unique<ElseTemplateToken>(pos, pre_space, post_space));
            } else if (content == "endif") {
                tokens.push_back(nonstd_make_unique<EndIfTemplateToken>(pos, pre_space, post_space));
            } else if (content == "endfor") {
                tokens.push_back(nonstd_make_unique<EndForTemplateToken>(pos, pre_space, post_space));
            } else if (content == "endblock") {
                tokens.push_back(nonstd_make_unique<EndBlockTemplateToken>(pos, pre_space, post_space));
            } else {
                throw std::runtime_error("Unknown block type: " + content);
            }
          } else {
            tokens.push_back(nonstd_make_unique<VariableTemplateToken>(pos, pre_space, post_space, std::move(parseExpression(content))));
          }
        } else {
          tokens.push_back(nonstd_make_unique<TextTemplateToken>(pos, pre_space, post_space, std::string(it, end)));
          break;
        }
      }
      return tokens;
    }

    std::unique_ptr<TemplateNode> parseTemplate(TemplateTokenIterator & it, const TemplateTokenIterator & end) const {
        std::vector<std::unique_ptr<TemplateNode>> children;
        auto done = false;
        while (it != end && !done) {
          const auto start = it;
          switch ((*it)->getType()) {
            case TemplateToken::Type::If: {
              std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<TemplateNode>>> cascade;

              auto if_token = dynamic_cast<IfTemplateToken*>((*(it++)).get());
              cascade.emplace_back(std::move(if_token->condition), std::move(parseTemplate(it, end)));

              while (it != end && (*it)->getType() == TemplateToken::Type::Elif) {
                  auto elif_token = dynamic_cast<ElifTemplateToken*>((*(it++)).get());
                  cascade.emplace_back(std::move(elif_token->condition), std::move(parseTemplate(it, end)));
              }

              if (it != end && (*it)->getType() == TemplateToken::Type::Else) {
                cascade.emplace_back(nullptr, std::move(parseTemplate(++it, end)));
              }
              if (it == end || (*(it++))->getType() != TemplateToken::Type::EndIf) {
                  throw (*start)->unterminated("if block");
              }
              children.emplace_back(nonstd_make_unique<IfNode>(std::move(cascade)));
              break;
            }
            case TemplateToken::Type::For: {
              auto for_token = dynamic_cast<ForTemplateToken*>((*it).get());
              auto body = parseTemplate(++it, end);
              if (it == end || (*(it++))->getType() != TemplateToken::Type::EndFor) {
                  throw (*start)->unterminated("for block");
              }
              children.emplace_back(nonstd_make_unique<ForNode>(for_token->var_names, std::move(for_token->iterable), std::move(for_token->condition), std::move(body), for_token->recursive));
              break;
            }
            case TemplateToken::Type::Text:
              children.emplace_back(nonstd_make_unique<TextNode>(dynamic_cast<TextTemplateToken*>((*(it++)).get())->text));
              break;
            case TemplateToken::Type::Variable:
              children.emplace_back(nonstd_make_unique<VariableNode>(std::move(dynamic_cast<VariableTemplateToken*>((*(it++)).get())->expr), std::vector<std::string>()));
              break;
            case TemplateToken::Type::Set: {
              auto set_token = dynamic_cast<SetTemplateToken*>((*(it++)).get());
              children.emplace_back(nonstd_make_unique<SetNode>(set_token->var_name, std::move(set_token->value)));
              break;
            }
            case TemplateToken::Type::Comment:
              // Ignore comments
              it++;
              break;
            case TemplateToken::Type::Block: {
              auto block_token = dynamic_cast<BlockTemplateToken*>((it++)->get());
              auto body = parseTemplate(++it, end);
              if (it == end || (*(it++))->getType() != TemplateToken::Type::EndBlock) {
                  throw (*start)->unterminated("named block");
              }
              children.emplace_back(nonstd_make_unique<BlockNode>(block_token->name, std::move(body)));
              break;
            }
            case TemplateToken::Type::EndBlock:
            case TemplateToken::Type::EndFor:
            case TemplateToken::Type::EndIf:
            case TemplateToken::Type::Else:
            case TemplateToken::Type::Elif:
              done = true;
              break;
            default:
              throw (*it)->unexpected("template");
          }
        }
        if (children.empty()) {
          return nonstd_make_unique<TextNode>(""); // Empty template!
        } else if (children.size() == 1) {
          return std::move(children[0]);
        } else {
          return nonstd_make_unique<SequenceNode>(std::move(children));
        }
    }

    std::string template_str;
    JinjaParser(const std::string& template_str) : template_str(template_str) {}

public:

    static std::unique_ptr<TemplateNode> parse(const std::string& template_str) {
        JinjaParser parser(template_str);

        auto tokens = parser.tokenize();
        TemplateTokenIterator it = tokens.begin();
        TemplateTokenIterator end = tokens.end();
        auto ret = parser.parseTemplate(it, end);
        if (it != end) {
            throw (*it)->unexpected("end of template");
        }
        return ret;
    }
};
