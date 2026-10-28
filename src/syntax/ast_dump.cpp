// Copyright 2026 The Phantasos Authors
// SPDX-License-Identifier: Apache-2.0
//
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

#include "syntax/ast_dump.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "support/utf8.hpp"

namespace phs {

using namespace ast;

namespace {

// Ordered tokens visit each source character at most once. Reset for callers
// dumping a reordered span or tokens from another file, preserving the public
// dump API's behavior without a persistent cache in the source manager.
class SourceCursor {
public:
    LineCol at(const SourceFile& source, u32 offset) {
        const std::string_view text = source.text;
        const u32 end = std::min(offset, static_cast<u32>(text.size()));
        if (source_ != &source || end < offset_) {
            source_ = &source;
            const auto line = std::ranges::upper_bound(source.line_starts, end);
            offset_ = *(line - 1);
            location_ = {static_cast<u32>(line - source.line_starts.begin()), 1};
        }
        while (offset_ < end) {
            if (text[offset_] == '\n') {
                ++location_.line;
                location_.col = 1;
                ++offset_;
            } else {
                offset_ += utf8_step(text.substr(offset_));
                ++location_.col;
            }
        }
        return location_;
    }

private:
    const SourceFile* source_ = nullptr;
    u32 offset_ = 0;
    LineCol location_{1, 1};
};

// Quotes a string the way the dumps show it. Valid UTF-8 is copied through and
// invalid bytes are escaped, so dumps are always valid UTF-8.
std::string quote(std::string_view s) {
    std::string out = "\"";
    auto it = std::back_inserter(out);
    while (!s.empty()) {
        const char c = s.front();
        if (static_cast<u8>(c) >= 0x80) {
            const auto ch = utf8_decode(s);
            if (ch)
                out += s.substr(0, ch->len);
            else
                std::format_to(it, "\\x{{{:02X}}}", static_cast<u8>(c));
            s.remove_prefix(ch ? ch->len : 1);
            continue;
        }
        s.remove_prefix(1);
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<u8>(c) < 0x20 || c == 0x7F)
                    std::format_to(it, "\\u{{{:x}}}", static_cast<u8>(c));
                else
                    out += c;
        }
    }
    out += '"';
    return out;
}

// Shortest round-trip form, always recognizable as a float ("1.0", not "1").
std::string fmt_float(double v) {
    std::string s = std::format("{}", v);
    if (!s.contains('.') && !s.contains('e') && !s.contains('n')) s += ".0";
    return s;
}

std::string fmt_char(u32 c) { return std::format("U+{:04X}", c); }

constexpr std::string_view unop_name(UnOp op) {
    switch (op) {
        case UnOp::Neg: return "Neg";
        case UnOp::Not: return "Not";
        case UnOp::BitNot: return "BitNot";
        case UnOp::Try: return "Try";
        case UnOp::TryOpt: return "TryOpt";
        case UnOp::Await: return "Await";
        case UnOp::Share: return "Share";
    }
    unreachable();
}

constexpr std::string_view binop_name(BinOp op) {
    switch (op) {
        case BinOp::Coalesce: return "Coalesce";
        case BinOp::Or: return "Or";
        case BinOp::And: return "And";
        case BinOp::Eq: return "Eq";
        case BinOp::Ne: return "Ne";
        case BinOp::Lt: return "Lt";
        case BinOp::Le: return "Le";
        case BinOp::Gt: return "Gt";
        case BinOp::Ge: return "Ge";
        case BinOp::Range: return "Range";
        case BinOp::RangeExcl: return "RangeExcl";
        case BinOp::Add: return "Add";
        case BinOp::Sub: return "Sub";
        case BinOp::WrapAdd: return "WrapAdd";
        case BinOp::WrapSub: return "WrapSub";
        case BinOp::BitOr: return "BitOr";
        case BinOp::BitXor: return "BitXor";
        case BinOp::Mul: return "Mul";
        case BinOp::Div: return "Div";
        case BinOp::Rem: return "Rem";
        case BinOp::WrapMul: return "WrapMul";
        case BinOp::BitAnd: return "BitAnd";
        case BinOp::Shl: return "Shl";
        case BinOp::Shr: return "Shr";
    }
    unreachable();
}

constexpr std::string_view assign_name(AssignOp op) {
    switch (op) {
        case AssignOp::Assign: return "=";
        case AssignOp::Add: return "+=";
        case AssignOp::Sub: return "-=";
        case AssignOp::Mul: return "*=";
        case AssignOp::Div: return "/=";
        case AssignOp::Rem: return "%=";
        case AssignOp::BitAnd: return "&=";
        case AssignOp::BitOr: return "|=";
        case AssignOp::BitXor: return "^=";
        case AssignOp::Shl: return "<<=";
        case AssignOp::Shr: return ">>=";
    }
    unreachable();
}

constexpr std::string_view cast_name(CastOp op) {
    switch (op) {
        case CastOp::Is: return "is";
        case CastOp::As: return "as";
        case CastOp::AsOpt: return "as?";
    }
    unreachable();
}

// Keyword spelling of a binding ("let"), and its dump title ("Let").
constexpr std::string_view binding_keyword(Binding b) {
    switch (b) {
        case Binding::None: return "";
        case Binding::Let: return "let";
        case Binding::Var: return "var";
        case Binding::Const: return "const";
    }
    unreachable();
}

constexpr std::string_view binding_title(Binding b) {
    switch (b) {
        case Binding::None: return "";
        case Binding::Let: return "Let";
        case Binding::Var: return "Var";
        case Binding::Const: return "Const";
    }
    unreachable();
}

constexpr std::string_view convention_prefix(Convention c) {
    switch (c) {
        case Convention::Default: return "";
        case Convention::Borrow: return "borrow ";
        case Convention::Inout: return "inout ";
        case Convention::Own: return "own ";
    }
    unreachable();
}

constexpr std::string_view visibility_suffix(Visibility v) {
    switch (v) {
        case Visibility::Default: return "";
        case Visibility::Public: return " vis=public";
        case Visibility::Internal: return " vis=internal";
        case Visibility::File: return " vis=file";
        case Visibility::Private: return " vis=private";
    }
    unreachable();
}

constexpr std::string_view type_decl_title(ItemKind k) {
    switch (k) {
        case ItemKind::Class: return "Class";
        case ItemKind::Struct: return "Struct";
        case ItemKind::Actor: return "Actor";
        case ItemKind::Enum: return "Enum";
        case ItemKind::Interface: return "Interface";
        default: unreachable();
    }
}

constexpr auto kModifierNames = std::to_array<std::pair<Modifier, std::string_view>>({
    {Modifier::Open, "open"},
    {Modifier::Final, "final"},
    {Modifier::Abstract, "abstract"},
    {Modifier::Sealed, "sealed"},
    {Modifier::Override, "override"},
    {Modifier::Static, "static"},
    {Modifier::Mut, "mut"},
    {Modifier::Async, "async"},
    {Modifier::Unsafe, "unsafe"},
});

// Formats each element with `fmt` and joins the results with `sep`.
template <std::ranges::input_range R, typename F>
std::string join(R&& range, std::string_view sep, F&& fmt) {
    return std::forward<R>(range) | std::views::transform(std::forward<F>(fmt)) | std::views::join_with(sep) |
           std::ranges::to<std::string>();
}

class Dumper {
public:
    explicit Dumper(const Interner& in) : in_(in) {}

    std::string run(const File& f) {
        line(0, "File");
        for (const Import& imp : f.imports)
            line(1, "Import {}{}", path(imp.path), imp.alias.sym.empty() ? "" : " as " + name(imp.alias));
        for (const Item* it : f.items) item(1, it);
        std::ranges::reverse(work_);
        while (!work_.empty()) {
            Task task = std::move(work_.back());
            work_.pop_back();
            const usize start = work_.size();
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, const Item*>) emit_item(task.depth, value);
                else if constexpr (std::is_same_v<T, const Stmt*>) emit_stmt(task.depth, value);
                else if constexpr (std::is_same_v<T, const Expr*>) emit_expr(task.depth, value);
                else {
                    out_.append(static_cast<usize>(task.depth) * 2, ' ');
                    out_ += value;
                    out_ += '\n';
                }
            }, task.value);
            // Emitters enqueue in source order; process their children before
            // the remaining siblings without using the C++ call stack.
            std::ranges::reverse(std::span(work_).subspan(start));
        }
        return std::move(out_);
    }

private:
    struct Task {
        int depth;
        std::variant<std::string, const Item*, const Stmt*, const Expr*> value;
    };

    void item(int depth, const Item* node) { work_.push_back({depth, node}); }
    void stmt(int depth, const Stmt* node) { work_.push_back({depth, node}); }
    void expr(int depth, const Expr* node) { work_.push_back({depth, node}); }

    // Queue one line indented by two spaces per level.
    template <typename... Args>
    void line(int depth, std::format_string<Args...> fmt, Args&&... args) {
        work_.push_back({depth, std::format(fmt, std::forward<Args>(args)...)});
    }

    std::string name(Name n) const { return n.sym.empty() ? "<missing>" : std::string(in_.str(n.sym)); }
    auto namer() const {
        return [this](Name n) { return name(n); };
    }
    auto typer() const {
        return [this](const TypeExpr* t) { return type(t); };
    }

    std::string path(const Path& p) const { return join(p.parts, ".", namer()); }

    // ---------------------------------------------------------- inline forms

    std::string type(const TypeExpr* t) const {
        // Postfix '?' can build an arbitrarily deep type without recursive
        // parsing, so inline type rendering also needs an explicit stack.
        std::vector<std::variant<std::string, const TypeExpr*>> pending{t};
        std::string out;
        const auto list = [&](List<TypeExpr*> types) {
            for (usize i = 0; i < types.size(); ++i) {
                if (i != 0) pending.emplace_back(", ");
                pending.emplace_back(types[i]);
            }
        };
        while (!pending.empty()) {
            auto next = std::move(pending.back());
            pending.pop_back();
            if (const auto* text = std::get_if<std::string>(&next)) {
                out += *text;
                continue;
            }
            const TypeExpr* node = std::get<const TypeExpr*>(next);
            if (!node) {
                out += "<none>";
                continue;
            }
            const usize start = pending.size();
            switch (node->kind) {
                case TypeKind::Path:
                    pending.emplace_back(path(node->path));
                    if (!node->args.empty()) {
                        pending.emplace_back("<");
                        list(node->args);
                        pending.emplace_back(">");
                    }
                    break;
                case TypeKind::Optional:
                    // A function type's result would take the '?' without parentheses.
                    if (node->inner->kind == TypeKind::Fun) pending.emplace_back("(");
                    pending.emplace_back(node->inner);
                    if (node->inner->kind == TypeKind::Fun) pending.emplace_back(")");
                    pending.emplace_back("?");
                    break;
                case TypeKind::Shared:
                case TypeKind::Weak:
                case TypeKind::Pointer:
                    pending.emplace_back(node->kind == TypeKind::Shared ? "shared " :
                                         node->kind == TypeKind::Weak ? "weak " : node->is_mut ? "*mut " : "*");
                    pending.emplace_back(node->inner);
                    break;
                case TypeKind::Array:
                    pending.emplace_back("[");
                    pending.emplace_back(node->inner);
                    pending.emplace_back("]");
                    break;
                case TypeKind::Unit: pending.emplace_back("()"); break;
                case TypeKind::Tuple:
                    pending.emplace_back("(");
                    list(node->args);
                    pending.emplace_back(")");
                    break;
                case TypeKind::SelfType: pending.emplace_back("Self"); break;
                case TypeKind::Fun:
                    pending.emplace_back("fun(");
                    list(node->args);
                    pending.emplace_back(")");
                    if (node->inner) {
                        pending.emplace_back(" -> ");
                        pending.emplace_back(node->inner);
                    }
                    if (node->throws) {
                        pending.emplace_back(node->throws_type ? " throws(" : " throws");
                        if (node->throws_type) {
                            pending.emplace_back(node->throws_type);
                            pending.emplace_back(")");
                        }
                    }
                    break;
                case TypeKind::Error: pending.emplace_back("<error>"); break;
            }
            std::ranges::reverse(std::span(pending).subspan(start));
        }
        return out;
    }

    std::string type_list(List<TypeExpr*> ts) const { return join(ts, ", ", typer()); }
    std::string bounds(List<TypeExpr*> ts) const { return join(ts, " & ", typer()); }

    std::string literal(const Expr* e) const {
        switch (e->kind) {
            case ExprKind::Int: return std::format("{}", cast<IntExpr>(e)->value);
            case ExprKind::Float: return fmt_float(cast<FloatExpr>(e)->value);
            case ExprKind::Char: return fmt_char(cast<CharExpr>(e)->value);
            case ExprKind::String: return quote(in_.str(cast<StringExpr>(e)->value));
            case ExprKind::Bool: return cast<BoolExpr>(e)->value ? "true" : "false";
            case ExprKind::None: return "none";
            case ExprKind::Unary: return "-" + literal(cast<UnaryExpr>(e)->operand);
            default: return "<error>";
        }
    }

    std::string pattern(const Pattern* p) const {
        std::vector<std::variant<std::string, const Pattern*>> pending{p};
        std::string out;
        const auto list = [&](List<Pattern*> patterns, std::string_view sep) {
            for (usize i = 0; i < patterns.size(); ++i) {
                if (i != 0) pending.emplace_back(std::string(sep));
                pending.emplace_back(patterns[i]);
            }
        };
        while (!pending.empty()) {
            auto next = std::move(pending.back());
            pending.pop_back();
            if (const auto* text = std::get_if<std::string>(&next)) {
                out += *text;
                continue;
            }
            const Pattern* node = std::get<const Pattern*>(next);
            const usize start = pending.size();
            switch (node->kind) {
                case PatKind::Wildcard: out += "_"; break;
                case PatKind::Literal: out += literal(cast<LiteralPat>(node)->literal); break;
                case PatKind::Binding: out += name(cast<BindingPat>(node)->name); break;
                case PatKind::Case: {
                    const auto* c = cast<CasePat>(node);
                    out += std::format("{}{}", c->leading_dot ? "." : "", join(c->path, ".", namer()));
                    if (c->has_args) {
                        out += "(";
                        list(c->args, ", ");
                        pending.emplace_back(")");
                    }
                    break;
                }
                case PatKind::Is: {
                    const auto* i = cast<IsPat>(node);
                    out += std::format("is {}{}", type(i->type), i->binding.sym.empty() ? "" : " as " + name(i->binding));
                    break;
                }
                case PatKind::Alt: list(cast<AltPat>(node)->alts, " | "); break;
                case PatKind::Error: out += "<error>"; break;
            }
            std::ranges::reverse(std::span(pending).subspan(start));
        }
        return out;
    }

    // ---------------------------------------------------------- items

    std::string header(std::string_view kind, const Item* it) const {
        std::string s{kind};
        if (!it->name.sym.empty()) s += " " + name(it->name);
        s += visibility_suffix(it->vis);
        auto modifiers = kModifierNames | std::views::filter([it](const auto& m) {
            return has_modifier(it->mods, m.first);
        });
        const std::string mods = join(modifiers, ",", [](const auto& m) { return m.second; });
        if (!mods.empty()) s += " mods=" + mods;
        return s;
    }

    void attrs(int d, const Item* it) {
        for (const Attribute& a : it->attrs) {
            line(d, "Attr {}{}", name(a.name), a.has_parens ? "()" : "");
            for (const AttrArg& arg : a.args) {
                if (arg.key.sym.empty()) {
                    expr(d + 1, arg.value);
                    continue;
                }
                line(d + 1, "Key {}", name(arg.key));
                expr(d + 2, arg.value);
            }
        }
    }

    void generics(int d, List<GenericParam> gs) {
        for (const GenericParam& g : gs)
            line(d, "Generic {}{}", name(g.name), g.bounds.empty() ? "" : ": " + bounds(g.bounds));
    }

    void where(int d, List<WhereClause> ws) {
        for (const WhereClause& w : ws) line(d, "Where {}: {}", type(w.type), bounds(w.bounds));
    }

    void params(int d, List<Param> ps) {
        for (const Param& p : ps) {
            const std::string binding = p.binding == Binding::None ? "" : std::format("{} ", binding_keyword(p.binding));
            line(d, "Param {}{}: {}{}", binding, name(p.name), convention_prefix(p.conv), type(p.type));
            if (p.default_value) {
                line(d + 1, "Default");
                expr(d + 2, p.default_value);
            }
        }
    }

    void throws_line(int d, bool throws, const TypeExpr* t) {
        if (!throws) return;
        if (t)
            line(d, "Throws {}", type(t));
        else
            line(d, "Throws");
    }

    void emit_item(int d, const Item* it) {
        attrs(d, it);
        switch (it->kind) {
            case ItemKind::Fun: {
                const auto* f = cast<FunItem>(it);
                line(d, "{}", header("Fun", it));
                generics(d + 1, f->sig.generics);
                params(d + 1, f->sig.params);
                if (f->sig.ret) line(d + 1, "Returns {}", type(f->sig.ret));
                throws_line(d + 1, f->sig.throws, f->sig.throws_type);
                where(d + 1, f->sig.where);
                if (f->body) {
                    line(d + 1, "{}", f->expr_body ? "ExprBody" : "Body");
                    expr(d + 2, f->body);
                }
                return;
            }
            case ItemKind::Class:
            case ItemKind::Struct:
            case ItemKind::Actor:
            case ItemKind::Interface:
            case ItemKind::Enum: {
                const auto* t = cast_type_decl(it);
                line(d, "{}", header(type_decl_title(it->kind), it));
                generics(d + 1, t->generics);
                if (t->has_ctor) {
                    line(d + 1, "Ctor");
                    params(d + 2, t->ctor_params);
                }
                for (const SuperEntry& s : t->supers) {
                    line(d + 1, "Super {}{}", type(s.type), s.has_args ? "()" : "");
                    args(d + 2, s.args);
                }
                where(d + 1, t->where);
                for (Item* m : t->members) item(d + 1, m);
                return;
            }
            case ItemKind::EnumCase: {
                const auto* c = cast<EnumCaseItem>(it);
                const auto field = [this](const CaseField& f) {
                    return f.label.sym.empty() ? type(f.type) : std::format("{}: {}", name(f.label), type(f.type));
                };
                if (c->has_fields)
                    line(d, "Case {}({})", name(c->name), join(c->fields, ", ", field));
                else
                    line(d, "Case {}", name(c->name));
                return;
            }
            case ItemKind::Var: {
                const auto* v = cast<VarItem>(it);
                line(d, "{}{}", header(binding_title(v->binding), it), v->type ? ": " + type(v->type) : "");
                if (v->init) expr(d + 1, v->init);
                return;
            }
            case ItemKind::Extern: {
                const auto* e = cast<ExternItem>(it);
                line(d, "{} {}", header("Extern", it), quote(in_.str(e->abi)));
                for (Item* m : e->items) item(d + 1, m);
                return;
            }
            case ItemKind::TypeAlias: {
                const auto* t = cast<TypeAliasItem>(it);
                line(d, "{}{}{}", header("TypeAlias", it), t->bounds.empty() ? "" : ": " + bounds(t->bounds),
                     t->type ? " = " + type(t->type) : "");
                generics(d + 1, t->generics);
                return;
            }
            case ItemKind::Init: {
                const auto* i = cast<InitItem>(it);
                line(d, "{}", header(i->has_params ? "Init" : "PrimaryInit", it));
                params(d + 1, i->params);
                throws_line(d + 1, i->throws, i->throws_type);
                expr(d + 1, i->body);
                return;
            }
            case ItemKind::Deinit:
                line(d, "{}", header("Deinit", it));
                expr(d + 1, cast<DeinitItem>(it)->body);
                return;
        }
    }

    // ---------------------------------------------------------- statements

    void cond(int d, const Cond& c) {
        if (c.is_let)
            line(d, "IfLet {}{}", name(c.name), c.type ? ": " + type(c.type) : "");
        else
            line(d, "Cond");
        expr(d + 1, c.expr);
    }

    void emit_stmt(int d, const Stmt* s) {
        switch (s->kind) {
            case StmtKind::Var: {
                const auto* v = cast<VarStmt>(s);
                std::string bound = name(v->name);
                if (!v->names.empty()) {
                    bound = "(";
                    for (usize i = 0; i < v->names.size(); ++i) bound += (i ? ", " : "") + name(v->names[i]);
                    bound += ")";
                }
                line(d, "{} {}{}", binding_title(v->binding), bound, v->type ? ": " + type(v->type) : "");
                if (v->init) expr(d + 1, v->init);
                return;
            }
            case StmtKind::Return: {
                const auto* r = cast<ReturnStmt>(s);
                line(d, "Return");
                if (r->value) expr(d + 1, r->value);
                return;
            }
            case StmtKind::Break: line(d, "Break"); return;
            case StmtKind::Continue: line(d, "Continue"); return;
            case StmtKind::Throw:
                line(d, "Throw");
                expr(d + 1, cast<ThrowStmt>(s)->value);
                return;
            case StmtKind::While: {
                const auto* w = cast<WhileStmt>(s);
                line(d, "While");
                cond(d + 1, w->cond);
                expr(d + 1, w->body);
                return;
            }
            case StmtKind::For: {
                const auto* f = cast<ForStmt>(s);
                line(d, "For {}", name(f->var));
                expr(d + 1, f->iter);
                expr(d + 1, f->body);
                return;
            }
            case StmtKind::Loop:
                line(d, "Loop");
                expr(d + 1, cast<LoopStmt>(s)->body);
                return;
            case StmtKind::Defer:
                line(d, "Defer");
                expr(d + 1, cast<DeferStmt>(s)->body);
                return;
            case StmtKind::Expr: expr(d, cast<ExprStmt>(s)->expr); return;
            case StmtKind::Assign: {
                const auto* a = cast<AssignStmt>(s);
                line(d, "Assign {}", assign_name(a->op));
                expr(d + 1, a->target);
                expr(d + 1, a->value);
                return;
            }
        }
    }

    // ---------------------------------------------------------- expressions

    void args(int d, List<Arg> as) {
        for (const Arg& a : as) {
            if (a.label.sym.empty()) {
                expr(d, a.value);
                continue;
            }
            line(d, "Label {}", name(a.label));
            expr(d + 1, a.value);
        }
    }

    void emit_expr(int d, const Expr* e) {
        if (!e) {
            line(d, "<null>");
            return;
        }
        switch (e->kind) {
            case ExprKind::Int: line(d, "Int {}", literal(e)); return;
            case ExprKind::Float: line(d, "Float {}", literal(e)); return;
            case ExprKind::Char: line(d, "Char {}", literal(e)); return;
            case ExprKind::String: line(d, "String {}", literal(e)); return;
            case ExprKind::Bool: line(d, "Bool {}", literal(e)); return;
            case ExprKind::Interp: {
                const auto* i = cast<InterpExpr>(e);
                line(d, "Interp");
                for (usize k = 0; k < i->texts.size(); ++k) {
                    line(d + 1, "Text {}", quote(in_.str(i->texts[k])));
                    if (k < i->exprs.size()) expr(d + 1, i->exprs[k]);
                }
                return;
            }
            case ExprKind::None: line(d, "None"); return;
            case ExprKind::Unit: line(d, "Unit"); return;
            case ExprKind::SelfValue: line(d, "SelfValue"); return;
            case ExprKind::Super: line(d, "Super"); return;
            case ExprKind::SelfType: line(d, "SelfType"); return;
            case ExprKind::Name: line(d, "Name {}", name(cast<NameExpr>(e)->name)); return;
            case ExprKind::ImplicitMember:
                line(d, "ImplicitMember .{}", name(cast<ImplicitMemberExpr>(e)->name));
                return;
            case ExprKind::Paren:
                line(d, "Paren");
                expr(d + 1, cast<ParenExpr>(e)->inner);
                return;
            case ExprKind::Array:
                line(d, "Array");
                for (const Expr* x : cast<ArrayExpr>(e)->elems) expr(d + 1, x);
                return;
            case ExprKind::Tuple:
                line(d, "Tuple");
                for (const Expr* x : cast<TupleExpr>(e)->elems) expr(d + 1, x);
                return;
            case ExprKind::TupleField: {
                const auto* f = cast<TupleFieldExpr>(e);
                line(d, "TupleField .{}", f->index);
                expr(d + 1, f->base);
                return;
            }
            case ExprKind::Unary: {
                const auto* u = cast<UnaryExpr>(e);
                line(d, "Unary {}", unop_name(u->op));
                expr(d + 1, u->operand);
                return;
            }
            case ExprKind::Binary: {
                const auto* b = cast<BinaryExpr>(e);
                line(d, "Binary {}", binop_name(b->op));
                expr(d + 1, b->lhs);
                expr(d + 1, b->rhs);
                return;
            }
            case ExprKind::Cast: {
                const auto* c = cast<CastExpr>(e);
                line(d, "Cast {} {}", cast_name(c->op), type(c->type));
                expr(d + 1, c->operand);
                return;
            }
            case ExprKind::Call: {
                const auto* c = cast<CallExpr>(e);
                line(d, "Call");
                expr(d + 1, c->callee);
                args(d + 1, c->args);
                return;
            }
            case ExprKind::Member: {
                const auto* m = cast<MemberExpr>(e);
                line(d, "Member .{}", name(m->name));
                expr(d + 1, m->base);
                return;
            }
            case ExprKind::OptMember: {
                const auto* m = cast<OptMemberExpr>(e);
                line(d, "OptMember ?.{}", name(m->name));
                expr(d + 1, m->base);
                return;
            }
            case ExprKind::Index: {
                const auto* i = cast<IndexExpr>(e);
                line(d, "Index");
                expr(d + 1, i->base);
                expr(d + 1, i->index);
                return;
            }
            case ExprKind::Unwrap:
                line(d, "Unwrap");
                expr(d + 1, cast<UnwrapExpr>(e)->operand);
                return;
            case ExprKind::GenericInst: {
                const auto* g = cast<GenericInstExpr>(e);
                line(d, "GenericInst <{}>", type_list(g->args));
                expr(d + 1, g->base);
                return;
            }
            case ExprKind::If: {
                const auto* i = cast<IfExpr>(e);
                line(d, "If");
                cond(d + 1, i->cond);
                expr(d + 1, i->then_block);
                if (i->else_branch) {
                    line(d + 1, "Else");
                    expr(d + 2, i->else_branch);
                }
                return;
            }
            case ExprKind::Match: {
                const auto* m = cast<MatchExpr>(e);
                line(d, "Match");
                expr(d + 1, m->scrutinee);
                for (const MatchArm& a : m->arms) {
                    line(d + 1, "Arm {}", pattern(a.pattern));
                    if (a.guard) {
                        line(d + 2, "Guard");
                        expr(d + 3, a.guard);
                    }
                    expr(d + 2, a.body);
                }
                return;
            }
            case ExprKind::Block:
                line(d, "Block");
                for (Stmt* s : cast<BlockExpr>(e)->stmts) stmt(d + 1, s);
                return;
            case ExprKind::Unsafe:
                line(d, "Unsafe");
                expr(d + 1, cast<UnsafeExpr>(e)->block);
                return;
            case ExprKind::Closure: {
                const auto* c = cast<ClosureExpr>(e);
                line(d, "Closure");
                for (const ClosureParam& p : c->params) {
                    if (p.type) line(d + 1, "Param {}: {}", name(p.name), type(p.type));
                    else line(d + 1, "Param {}", name(p.name));
                }
                expr(d + 1, c->body);
                return;
            }
            case ExprKind::Error: line(d, "<error>"); return;
        }
    }

    const Interner& in_;
    std::vector<Task> work_;
    std::string out_;
};

}  // namespace

std::string dump_tokens(std::span<const Token> tokens, const Interner& in, const SourceManager& sm) {
    std::string out;
    auto it = std::back_inserter(out);
    SourceCursor locations;
    for (const Token& t : tokens) {
        const LineCol lc = locations.at(sm.file(t.span.file), t.span.lo);
        std::format_to(it, "{}:{} {}", lc.line, lc.col, tok_name(t.kind));
        switch (t.kind) {
            case Tok::Ident: std::format_to(it, " {}", in.str(t.symbol())); break;
            case Tok::IntLit: std::format_to(it, " {}", t.int_value()); break;
            case Tok::FloatLit: std::format_to(it, " {}", fmt_float(t.float_value())); break;
            case Tok::CharLit: std::format_to(it, " {}", fmt_char(t.char_value())); break;
            case Tok::StringLit:
            case Tok::StringHead:
            case Tok::StringMiddle:
            case Tok::StringTail: std::format_to(it, " {}", quote(in.str(t.symbol()))); break;
            default: break;
        }
        if (t.space_before && t.kind != Tok::Newline && t.kind != Tok::Eof) out += " (sp)";
        out += '\n';
    }
    return out;
}

std::string dump_ast(const File& file, const Interner& in) { return Dumper(in).run(file); }

}  // namespace phs
