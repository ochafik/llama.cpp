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

bool is_true(const json& j) {
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
};

class LiteralExpr : public Expression {
    json value;
public:
    LiteralExpr(const json& v) : value(v) {}
    json evaluate(json&) const override { return value; }
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

class BinaryOpExpr : public Expression {
public:
    enum class Op { StrConcat, Add, Sub, Mul, Div, Mod, Pow, Eq, Ne, Lt, Gt, Le, Ge, And, Or, In, Is };
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
            case Op::Div:       return l / r;
            case Op::Mod:       return l.get<int>() % r.get<int>();
            case Op::Pow:       return std::pow(l.get<double>(), r.get<double>());
            case Op::Eq:        return l == r;
            case Op::Ne:        return l != r;
            case Op::Lt:        return l < r;
            case Op::Gt:        return l > r;
            case Op::Le:        return l <= r;
            case Op::Ge:        return l >= r;
            case Op::And:       return is_true(l) && is_true(r);
            case Op::Or:        return is_true(l) || is_true(r);
            case Op::In:        return r.is_array() && r.find(l) != r.end();
            default:            break;
        }
        throw std::runtime_error("Unknown binary operator");
    }
};

class MethodCallExpr : public Expression {
    std::unique_ptr<Expression> object; // If nullptr, this is a function call
    std::string method;
    std::vector<std::unique_ptr<Expression>> args;
public:
    MethodCallExpr(std::unique_ptr<Expression> && obj, const std::string& m, std::vector<std::unique_ptr<Expression>> && a)
        : object(std::move(obj)), method(m), args(std::move(a)) {}
    bool is_function_call() const { return object == nullptr; }
    const std::string& get_method() const { return method; }
    const std::vector<std::unique_ptr<Expression>>& get_args() const { return args; }
    json evaluate(json& context) const override {
        json obj = object->evaluate(context);
        if (method == "append" && obj.is_array()) {
            for (const auto& arg : args) {
                obj.push_back(arg->evaluate(context));
            }
            return obj;
        }
        throw std::runtime_error("Unknown method: " + method);
    }
};

class PipeExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> parts;
public:
    PipeExpr(std::vector<std::unique_ptr<Expression>> && p) : parts(std::move(p)) {}
    json evaluate(json& context) const override {
        json result;
        for (const auto& part : parts) {
            if (auto mc = dynamic_cast<MethodCallExpr*>(part.get())) {
                if (!mc->is_function_call()) {
                  throw std::runtime_error("Method call in pipe expression must be a function call: " + mc->get_method());
                }
                if (mc->get_method() == "tojson") {
                  auto indent = mc->get_args().empty() ? 0 : mc->get_args()[0]->evaluate(context).get<int>();
                  result = result.dump(indent);
                } else if (mc->get_method() == "join") {
                  auto sep = mc->get_args().empty() ? "" : mc->get_args()[0]->evaluate(context).get<std::string>();
                  std::ostringstream oss;
                  auto first = true;
                  for (const auto& item : result) {
                    if (first) first = false;
                    else oss << sep;
                    oss << item.get<std::string>();
                  }
                  result = oss.str();
                } else {
                  throw std::runtime_error("Unknown function in pipe: " + mc->get_method());
                }
            } else {
                result = part->evaluate(context);
            }
        }
        return result;
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
        if (is_true(branch.first->evaluate(context))) {
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
        if (!condition || is_true(condition->evaluate(context))) {
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
    // std::vector<std::unique_ptr<TemplateNode>> ast;

    /**
     * Parsing functions call each other in the right priority order.
     *
     * The parsing functions are:
     * - parseFullExpression
     * - 
     * - parsePipe
     * - parseExpression
     * - parseEqualityExpression
     * - parseComparisonExpression
     * - parseAdditionExpression
     * - parseMultiplicationExpression
     * - parseUnaryExpression
     * - parsePrimaryExpression
     * - parseMethodCall
     * 
     */

    std::unique_ptr<Expression> parseExpression(const std::string& expr) const {
        std::regex binary_op_regex(R"((.+?)\s*(~|==|!=|<|>|<=|>=|\+|-|\*\*?|/|%|\bin\b|\bis\b)\s*(.+))");
        std::regex method_call_regex(R"((\w+)\.(\w+)\((.*?)\))");
        std::smatch match;

        if (std::regex_match(expr, match, binary_op_regex)) {
            auto left = parseExpression(match[1].str());
            auto right = parseExpression(match[3].str());
            std::string op_str = match[2].str();
            BinaryOpExpr::Op op;
            if (op_str == "~") op = BinaryOpExpr::Op::StrConcat;
            else if (op_str == "+") op = BinaryOpExpr::Op::Add;
            else if (op_str == "-") op = BinaryOpExpr::Op::Sub;
            else if (op_str == "*") op = BinaryOpExpr::Op::Mul;
            else if (op_str == "/") op = BinaryOpExpr::Op::Div;
            else if (op_str == "%") op = BinaryOpExpr::Op::Mod;
            else if (op_str == "**") op = BinaryOpExpr::Op::Pow;
            else if (op_str == "==") op = BinaryOpExpr::Op::Eq;
            else if (op_str == "!=") op = BinaryOpExpr::Op::Ne;
            else if (op_str == "<") op = BinaryOpExpr::Op::Lt;
            else if (op_str == ">") op = BinaryOpExpr::Op::Gt;
            else if (op_str == "<=") op = BinaryOpExpr::Op::Le;
            else if (op_str == ">=") op = BinaryOpExpr::Op::Ge;
            else if (op_str == "in") op = BinaryOpExpr::Op::In;
            else if (op_str == "is") op = BinaryOpExpr::Op::Is;
            else throw std::runtime_error("Unknown binary operator: " + op_str);
            return nonstd_make_unique<BinaryOpExpr>(std::move(left), std::move(right), op);
        } else if (std::regex_match(expr, match, method_call_regex)) {
            auto object = nonstd_make_unique<VariableExpr>(match[1].str());
            std::string method = match[2].str();
            std::vector<std::unique_ptr<Expression>> args;
            std::string args_str = match[3].str();
            if (!args_str.empty()) {
                args.push_back(parseExpression(args_str));
            }
            return nonstd_make_unique<MethodCallExpr>(std::move(object), method, std::move(args));
        }
        
        // Check if it's a boolean
        if (expr == "true") return nonstd_make_unique<LiteralExpr>(true);
        if (expr == "false") return nonstd_make_unique<LiteralExpr>(false);

        // TODO: parse lists, strings, object literals, index access.
        
        // Check if it's a number
        try {
            return nonstd_make_unique<LiteralExpr>(std::stod(expr));
        } catch (const std::invalid_argument&) {
            // Not a number, treat as a variable
            return nonstd_make_unique<VariableExpr>(expr);
        }
    }

    static SpaceHandling parseSpaceHandling(const std::string& s) {
        if (s == "-") return SpaceHandling::Strip;
        if (s == "~") return SpaceHandling::KeepLines;
        return SpaceHandling::Keep;
    }

    using TemplateTokenVector = std::vector<std::unique_ptr<TemplateToken>>;
    using TemplateTokenIterator = TemplateTokenVector::const_iterator;

    TemplateTokenVector tokenize(const std::string& template_str) const {
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

public:
    JinjaParser() {}

    std::unique_ptr<TemplateNode> parse(const std::string& template_str) const {
        auto tokens = tokenize(template_str);

        TemplateTokenIterator it = tokens.begin();
        TemplateTokenIterator end = tokens.end();
        auto ret = parseTemplate(it, end);
        if (it != end) {
            throw (*it)->unexpected("end of template");
        }
        return ret;
    }
};
