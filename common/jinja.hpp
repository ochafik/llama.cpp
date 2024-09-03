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

// Forward declarations
class ASTNode;
class Expression;

// AST Node types
enum class NodeType {
    Sequence,
    Text,
    Variable,
    If,
    For,
    Set,
    Expression
};

class ASTNode {
    NodeType type;
protected:
    ASTNode(NodeType t) : type(t) {}
public:
    virtual ~ASTNode() = default;
    virtual void render(std::ostringstream& oss, json& context) const = 0;
    NodeType getType() const { return type; }

    std::string render(json & context) const {
        std::ostringstream oss;
        render(oss, context);
        return oss.str();
    }
};

class SequenceNode : public ASTNode {
    std::vector<std::unique_ptr<ASTNode>> children;
public:
    SequenceNode(std::vector<std::unique_ptr<ASTNode>> && c) : ASTNode(NodeType::Sequence), children(std::move(c)) {}
    void render(std::ostringstream& oss, json& context) const override {
        for (const auto& child : children) {
            child->render(oss, context);
        }
    }
};

class TextNode : public ASTNode {
    std::string text;
public:
    TextNode(const std::string& t) : ASTNode(NodeType::Text), text(t) {}
    void render(std::ostringstream& oss, json&) const override { oss << text; }
};

class VariableNode : public ASTNode {
    std::unique_ptr<Expression> expr;
    std::vector<std::string> filters;
public:
    VariableNode(std::unique_ptr<Expression> && e, std::vector<std::string> && f) 
        : ASTNode(NodeType::Variable), expr(std::move(e)), filters(std::move(f)) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class IfNode : public ASTNode {
    std::unique_ptr<Expression> condition;
    std::vector<std::unique_ptr<ASTNode>> true_branch;
    std::vector<std::unique_ptr<ASTNode>> false_branch;
public:
    IfNode(std::unique_ptr<Expression> && cond,
           std::vector<std::unique_ptr<ASTNode>> && tb,
           std::vector<std::unique_ptr<ASTNode>> && fb)
        : ASTNode(NodeType::If), condition(std::move(cond)), true_branch(std::move(tb)), false_branch(std::move(fb)) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class ForNode : public ASTNode {
    std::string var_name;
    std::unique_ptr<Expression> iterable;
    std::vector<std::unique_ptr<ASTNode>> body;
public:
    ForNode(const std::string& vn, std::unique_ptr<Expression> && iter,
            std::vector<std::unique_ptr<ASTNode>> && b)
            : ASTNode(NodeType::For), var_name(vn), iterable(std::move(iter)), body(std::move(b)) {}
    void render(std::ostringstream& oss, json& context) const override;
};

class SetNode : public ASTNode {
    std::string var_name;
    std::unique_ptr<Expression> value;
public:
    SetNode(const std::string& vn, std::unique_ptr<Expression> && v)
      : ASTNode(NodeType::Set), var_name(vn), value(std::move(v)) {}
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
    enum class Op { StrConcat, Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Gt, Le, Ge, And, Or, In };
private:
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    Op op;
public:
    BinaryOpExpr(std::unique_ptr<Expression> && l, std::unique_ptr<Expression> && r, Op o)
        : left(std::move(l)), right(std::move(r)), op(o) {}
    json evaluate(json& context) const override {
        json l = left->evaluate(context);
        json r = right->evaluate(context);
        switch (op) {
            case Op::StrConcat: return l.get<std::string>() + r.get<std::string>();
            case Op::Add: return l + r;
            case Op::Sub: return l - r;
            case Op::Mul: return l * r;
            case Op::Div: return l / r;
            case Op::Mod: return l.get<int>() % r.get<int>();
            case Op::Eq: return l == r;
            case Op::Ne: return l != r;
            case Op::Lt: return l < r;
            case Op::Gt: return l > r;
            case Op::Le: return l <= r;
            case Op::Ge: return l >= r;
            case Op::And: return l.get<bool>() && r.get<bool>();
            case Op::Or: return l.get<bool>() || r.get<bool>();
            case Op::In: return r.is_array() && r.find(l) != r.end();
        }
        throw std::runtime_error("Unknown binary operator");
    }
};

class MethodCallExpr : public Expression {
    std::unique_ptr<Expression> object;
    std::string method;
    std::vector<std::unique_ptr<Expression>> args;
public:
    MethodCallExpr(std::unique_ptr<Expression> && obj, const std::string& m, std::vector<std::unique_ptr<Expression>> && a)
        : object(std::move(obj)), method(m), args(std::move(a)) {}
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

// Helper function to render a vector of nodes
void renderNodes(std::ostringstream& oss, const std::vector<std::unique_ptr<ASTNode>>& nodes, json& context) {
    for (const auto& node : nodes) {
        node->render(oss, context);
    }
}

// Implement VariableNode::render
void VariableNode::render(std::ostringstream& oss, json& context) const {
    json result = expr->evaluate(context);
    for (const auto& filter : filters) {
        if (filter == "join") {
            if (result.is_array()) {
                oss << result.dump();
            }
        } else if (filter == "tojson") {
            oss << result.dump(2);  // Pretty print JSON with 2-space indent
        } else {
            throw std::runtime_error("Unknown filter: " + filter);
        }
    }
    if (filters.empty()) {
        if (result.is_string()) {
            oss << result.get<std::string>();
        } else {
            oss << result.dump();
        }
    }
}

// Implement IfNode::render
void IfNode::render(std::ostringstream& oss, json& context) const {
    if (condition->evaluate(context).get<bool>()) {
        renderNodes(oss, true_branch, context);
    } else {
        renderNodes(oss, false_branch, context);
    }
}

// Implement ForNode::render
void ForNode::render(std::ostringstream& oss, json& context) const {
    json loop_var = iterable->evaluate(context);
    if (loop_var.is_array()) {
        json original_context = context;
        for (const auto& item : loop_var) {
            context[var_name] = item;
            renderNodes(oss, body, context);
        }
        context = original_context;
    }
}

// Implement SetNode::render
void SetNode::render(std::ostringstream&, json& context) const {
    context[var_name] = value->evaluate(context);
}




class JinjaParser {
private:
    // std::vector<std::unique_ptr<ASTNode>> ast;

    std::unique_ptr<Expression> parseExpression(const std::string& expr) {
        std::regex binary_op_regex(R"((.+?)\s*(~|==|!=|<|>|<=|>=|\+|-|\*|/|%|in)\s*(.+))");
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
            else if (op_str == "==") op = BinaryOpExpr::Op::Eq;
            else if (op_str == "!=") op = BinaryOpExpr::Op::Ne;
            else if (op_str == "<") op = BinaryOpExpr::Op::Lt;
            else if (op_str == ">") op = BinaryOpExpr::Op::Gt;
            else if (op_str == "<=") op = BinaryOpExpr::Op::Le;
            else if (op_str == ">=") op = BinaryOpExpr::Op::Ge;
            else if (op_str == "in") op = BinaryOpExpr::Op::In;
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
        
        // Check if it's a number
        try {
            return nonstd_make_unique<LiteralExpr>(std::stod(expr));
        } catch (const std::invalid_argument&) {
            // Not a number, treat as a variable
            return nonstd_make_unique<VariableExpr>(expr);
        }
    }

    std::pair<std::vector<std::unique_ptr<ASTNode>>, std::vector<std::unique_ptr<ASTNode>>>
    parseIfBlock(std::string::const_iterator& start, std::string::const_iterator end) {
        std::vector<std::unique_ptr<ASTNode>> true_branch;
        std::vector<std::unique_ptr<ASTNode>> false_branch;
        std::regex else_regex(R"(\{%\s*else\s*%\})");
        std::regex endif_regex(R"(\{%\s*endif\s*%\})");
        std::smatch match;

        auto current_branch = &true_branch;

        while (start != end) {
            if (std::regex_search(start, end, match, else_regex)) {
                if (match.position() > 0) {
                    current_branch->push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                current_branch = &false_branch;
                start += match.position() + match.length();
            } else if (std::regex_search(start, end, match, endif_regex)) {
                if (match.position() > 0) {
                    current_branch->push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                start += match.position() + match.length();
                break;
            } else {
                auto nested = parse(std::string(start, end));
                current_branch->push_back(std::move(nested));
                break;
            }
        }

        return {std::move(true_branch), std::move(false_branch)};
    }

    std::vector<std::unique_ptr<ASTNode>> parseForBlock(std::string::const_iterator& start, std::string::const_iterator end) {
        std::vector<std::unique_ptr<ASTNode>> body;
        std::regex endfor_regex(R"(\{%\s*endfor\s*%\})");
        std::smatch match;

        while (start != end) {
            if (std::regex_search(start, end, match, endfor_regex)) {
                if (match.position() > 0) {
                    body.push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                start += match.position() + match.length();
                break;
            } else {
                auto nested = parse(std::string(start, end));
                body.push_back(std::move(nested));
                break;
            }
        }

        return body;
    }

public:
    JinjaParser() {}

    std::unique_ptr<ASTNode> parse(const std::string& template_str) {
        std::regex var_regex(R"(\{\{\s*(.*?)\s*\}\})");
        std::regex if_regex(R"(\{%\s*if\s+(.*?)\s*%\})");
        std::regex else_regex(R"(\{%\s*else\s*%\})");
        std::regex endif_regex(R"(\{%\s*endif\s*%\})");
        std::regex for_regex(R"(\{%\s*for\s+(\w+)\s+in\s+(.*?)\s*%\})");
        std::regex endfor_regex(R"(\{%\s*endfor\s*%\})");
        std::regex set_regex(R"(\{%\s*set\s+(\w+)\s*=\s*(.*?)\s*%\})");

        std::string::const_iterator start = template_str.begin();
        std::string::const_iterator end = template_str.end();
        std::smatch match;

        std::vector<std::unique_ptr<ASTNode>> children;

        while (start != end) {
            if (std::regex_search(start, end, match, var_regex)) {
                if (match.position() > 0) {
                    children.push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                std::string var_expr = match[1].str();
                std::vector<std::string> filters;
                size_t pipe_pos = var_expr.find('|');
                if (pipe_pos != std::string::npos) {
                    std::string filters_str = var_expr.substr(pipe_pos + 1);
                    var_expr = var_expr.substr(0, pipe_pos);
                    std::istringstream iss(filters_str);
                    std::string filter;
                    while (std::getline(iss, filter, '|')) {
                        // Strip leading and trailing whitespaces
                        filter = std::regex_replace(filter, std::regex("^\\s+|\\s+$"), "");
                        filters.push_back(filter);
                    }
                }
                children.push_back(nonstd_make_unique<VariableNode>(parseExpression(var_expr), std::move(filters)));
                start += match.position() + match.length();
            } else if (std::regex_search(start, end, match, set_regex)) {
                if (match.position() > 0) {
                    children.push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                std::string var_name = match[1].str();
                auto value = parseExpression(match[2].str());
                children.push_back(nonstd_make_unique<SetNode>(var_name, std::move(value)));
                start += match.position() + match.length();
            } else if (std::regex_search(start, end, match, if_regex)) {
                if (match.position() > 0) {
                    children.push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                start += match.position() + match.length();
                auto condition = parseExpression(match[1].str());
                auto if_block = parseIfBlock(start, end);
                const auto & true_branch = if_block.first;
                const auto & false_branch = if_block.second;
                // children.push_back(nonstd_make_unique<IfNode>(std::move(condition), std::move(true_branch), std::move(false_branch)));
                // children.emplace_back(nonstd_make_unique<IfNode>(std::move(condition), std::move(true_branch), std::move(false_branch)));
            } else if (std::regex_search(start, end, match, for_regex)) {
                if (match.position() > 0) {
                    children.push_back(nonstd_make_unique<TextNode>(std::string(start, start + match.position())));
                }
                start += match.position() + match.length();
                std::string var_name = match[1].str();
                auto iterable = parseExpression(match[2].str());
                auto body = parseForBlock(start, end);
                children.push_back(nonstd_make_unique<ForNode>(var_name, std::move(iterable), std::move(body)));
            } else {
                children.push_back(nonstd_make_unique<TextNode>(std::string(start, end)));
                break;
            }
        }
        return nonstd_make_unique<SequenceNode>(std::move(children));
    }
};
