/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Spreadsheet.h"
#include "JSIntegration.h"
#include "Workbook.h"
#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/ScopeGuard.h>
#include <AK/TemporaryChange.h>
#include <AK/URL.h>
#include <LibCore/File.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <ctype.h>
#include <unistd.h>

namespace Spreadsheet {

Sheet::Sheet(const StringView& name, Workbook& workbook)
    : Sheet(workbook)
{
    m_name = name;

    for (size_t i = 0; i < default_row_count; ++i)
        add_row();

    for (size_t i = 0; i < default_column_count; ++i)
        add_column();
}

Sheet::Sheet(Workbook& workbook)
    : m_workbook(workbook)
{
    JS::DeferGC defer_gc(m_workbook.interpreter().heap());
    m_global_object = m_workbook.interpreter().heap().allocate_without_global_object<SheetGlobalObject>(*this);
    global_object().initialize_global_object();
    global_object().put("workbook", m_workbook.workbook_object());
    global_object().put("thisSheet", &global_object()); // Self-reference is unfortunate, but required.

    // Sadly, these have to be evaluated once per sheet.
    auto file_or_error = Core::File::open("/res/js/Spreadsheet/runtime.js", Core::OpenMode::ReadOnly);
    if (!file_or_error.is_error()) {
        auto buffer = file_or_error.value()->read_all();
        JS::Parser parser { JS::Lexer(buffer) };
        if (parser.has_errors()) {
            warnln("Spreadsheet: Failed to parse runtime code");
            parser.print_errors();
        } else {
            interpreter().run(global_object(), parser.parse_program());
            if (auto* exception = interpreter().exception()) {
                warnln("Spreadsheet: Failed to run runtime code:");
                for (auto& traceback_frame : exception->traceback()) {
                    auto& function_name = traceback_frame.function_name;
                    auto& source_range = traceback_frame.source_range;
                    dbgln("  {} at {}:{}:{}", function_name, source_range.filename, source_range.start.line, source_range.start.column);
                }
                interpreter().vm().clear_exception();
            }
        }
    }
}

Sheet::~Sheet()
{
}

JS::Interpreter& Sheet::interpreter() const
{
    return m_workbook.interpreter();
}

size_t Sheet::add_row()
{
    return m_rows++;
}

static size_t convert_from_string(StringView str, unsigned base = 26, StringView map = {})
{
    if (map.is_null())
        map = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    VERIFY(base >= 2 && base <= map.length());

    size_t value = 0;
    for (size_t i = str.length(); i > 0; --i) {
        auto digit_value = map.find_first_of(str[i - 1]).value_or(0);
        // NOTE: Refer to the note in `String::bijective_base_from()'.
        if (i == str.length() && str.length() > 1)
            ++digit_value;
        value = value * base + digit_value;
    }

    return value;
}

String Sheet::add_column()
{
    auto next_column = String::bijective_base_from(m_columns.size());
    m_columns.append(next_column);
    return next_column;
}

void Sheet::update()
{
    if (m_should_ignore_updates) {
        m_update_requested = true;
        return;
    }
    m_visited_cells_in_update.clear();
    Vector<Cell&> cells_copy;

    // Grab a copy as updates might insert cells into the table.
    for (auto& it : m_cells) {
        if (it.value->dirty()) {
            cells_copy.append(*it.value);
            m_workbook.set_dirty(true);
        }
    }

    for (auto& cell : cells_copy)
        update(cell);

    m_visited_cells_in_update.clear();
}

void Sheet::update(Cell& cell)
{
    if (m_should_ignore_updates) {
        m_update_requested = true;
        return;
    }
    if (cell.dirty()) {
        if (has_been_visited(&cell)) {
            // This may be part of an cyclic reference chain,
            // so just ignore it.
            cell.clear_dirty();
            return;
        }
        m_visited_cells_in_update.set(&cell);
        cell.update_data({});
    }
}

Sheet::ValueAndException Sheet::evaluate(const StringView& source, Cell* on_behalf_of)
{
    TemporaryChange cell_change { m_current_cell_being_evaluated, on_behalf_of };
    ScopeGuard clear_exception { [&] { interpreter().vm().clear_exception(); } };

    auto parser = JS::Parser(JS::Lexer(source));
    auto program = parser.parse_program();
    if (parser.has_errors() || interpreter().exception())
        return { JS::js_undefined(), interpreter().exception() };

    interpreter().run(global_object(), program);
    if (interpreter().exception()) {
        auto exc = interpreter().exception();
        return { JS::js_undefined(), exc };
    }

    auto value = interpreter().vm().last_value();
    if (value.is_empty())
        return { JS::js_undefined(), {} };
    return { value, {} };
}

Cell* Sheet::at(const StringView& name)
{
    auto pos = parse_cell_name(name);
    if (pos.has_value())
        return at(pos.value());

    return nullptr;
}

Cell* Sheet::at(const Position& position)
{
    auto it = m_cells.find(position);

    if (it == m_cells.end())
        return nullptr;

    return it->value;
}

Optional<Position> Sheet::parse_cell_name(const StringView& name) const
{
    GenericLexer lexer(name);
    auto col = lexer.consume_while(isalpha);
    auto row = lexer.consume_while(isdigit);

    if (!lexer.is_eof() || row.is_empty() || col.is_empty())
        return {};

    auto it = m_columns.find(col);
    if (it == m_columns.end())
        return {};

    return Position { it.index(), row.to_uint().value() };
}

Optional<size_t> Sheet::column_index(const StringView& column_name) const
{
    auto index = convert_from_string(column_name);
    if (m_columns.size() <= index || m_columns[index] != column_name) {
        auto it = m_columns.find(column_name);
        if (it == m_columns.end())
            return {};
        index = it.index();
    }

    return index;
}

Optional<String> Sheet::column_arithmetic(const StringView& column_name, int offset)
{
    auto maybe_index = column_index(column_name);
    if (!maybe_index.has_value())
        return {};

    if (offset < 0 && maybe_index.value() < (size_t)(0 - offset))
        return m_columns.first();

    auto index = maybe_index.value() + offset;
    if (m_columns.size() > index)
        return m_columns[index];

    for (size_t i = m_columns.size(); i <= index; ++i)
        add_column();

    return m_columns.last();
}

Cell* Sheet::from_url(const URL& url)
{
    auto maybe_position = position_from_url(url);
    if (!maybe_position.has_value())
        return nullptr;

    return at(maybe_position.value());
}

Optional<Position> Sheet::position_from_url(const URL& url) const
{
    if (!url.is_valid()) {
        dbgln("Invalid url: {}", url.to_string());
        return {};
    }

    if (url.protocol() != "spreadsheet" || url.host() != "cell") {
        dbgln("Bad url: {}", url.to_string());
        return {};
    }

    // FIXME: Figure out a way to do this cross-process.
    VERIFY(url.path() == String::formatted("/{}", getpid()));

    return parse_cell_name(url.fragment());
}

Position Sheet::offset_relative_to(const Position& base, const Position& offset, const Position& offset_base) const
{
    if (offset.column >= m_columns.size()) {
        dbgln("Column '{}' does not exist!", offset.column);
        return base;
    }
    if (offset_base.column >= m_columns.size()) {
        dbgln("Column '{}' does not exist!", offset_base.column);
        return base;
    }
    if (base.column >= m_columns.size()) {
        dbgln("Column '{}' does not exist!", base.column);
        return offset;
    }

    auto new_column = offset.column + base.column - offset_base.column;
    auto new_row = offset.row + base.row - offset_base.row;

    return { new_column, new_row };
}

void Sheet::copy_cells(Vector<Position> from, Vector<Position> to, Optional<Position> resolve_relative_to, CopyOperation copy_operation)
{
    auto copy_to = [&](auto& source_position, Position target_position) {
        auto& target_cell = ensure(target_position);
        auto* source_cell = at(source_position);

        if (!source_cell) {
            target_cell.set_data("");
            return;
        }

        target_cell.copy_from(*source_cell);
        if (copy_operation == CopyOperation::Cut)
            source_cell->set_data("");
    };

    if (from.size() == to.size()) {
        auto from_it = from.begin();
        // FIXME: Ordering.
        for (auto& position : to)
            copy_to(*from_it++, position);

        return;
    }

    if (to.size() == 1) {
        // Resolve each index as relative to the first index offset from the selection.
        auto& target = to.first();

        for (auto& position : from) {
            dbgln_if(COPY_DEBUG, "Paste from '{}' to '{}'", position.to_url(*this), target.to_url(*this));
            copy_to(position, resolve_relative_to.has_value() ? offset_relative_to(target, position, resolve_relative_to.value()) : target);
        }

        return;
    }

    if (from.size() == 1) {
        // Fill the target selection with the single cell.
        auto& source = from.first();
        for (auto& position : to) {
            dbgln_if(COPY_DEBUG, "Paste from '{}' to '{}'", source.to_url(*this), position.to_url(*this));
            copy_to(source, resolve_relative_to.has_value() ? offset_relative_to(position, source, resolve_relative_to.value()) : position);
        }
        return;
    }

    // Just disallow misaligned copies.
    dbgln("Cannot copy {} cells to {} cells", from.size(), to.size());
}

RefPtr<Sheet> Sheet::from_json(const JsonObject& object, Workbook& workbook)
{
    auto sheet = adopt_ref(*new Sheet(workbook));
    auto rows = object.get("rows").to_u32(default_row_count);
    auto columns = object.get("columns");
    auto name = object.get("name").as_string_or("Sheet");
    if (object.has("cells") && !object.has_object("cells"))
        return {};

    sheet->set_name(name);

    for (size_t i = 0; i < max(rows, (unsigned)Sheet::default_row_count); ++i)
        sheet->add_row();

    // FIXME: Better error checking.
    if (columns.is_array()) {
        columns.as_array().for_each([&](auto& value) {
            sheet->m_columns.append(value.as_string());
            return IterationDecision::Continue;
        });
    }

    if (sheet->m_columns.size() < default_column_count && sheet->columns_are_standard()) {
        for (size_t i = sheet->m_columns.size(); i < default_column_count; ++i)
            sheet->add_column();
    }

    auto json = sheet->interpreter().global_object().get("JSON");
    auto& parse_function = json.as_object().get("parse").as_function();

    auto read_format = [](auto& format, const auto& obj) {
        if (auto value = obj.get("foreground_color"); value.is_string())
            format.foreground_color = Color::from_string(value.as_string());
        if (auto value = obj.get("background_color"); value.is_string())
            format.background_color = Color::from_string(value.as_string());
    };

    if (object.has_object("cells")) {
        object.get("cells").as_object().for_each_member([&](auto& name, JsonValue const& value) {
            auto position_option = sheet->parse_cell_name(name);
            if (!position_option.has_value())
                return IterationDecision::Continue;

            auto position = position_option.value();
            auto& obj = value.as_object();
            auto kind = obj.get("kind").as_string_or("LiteralString") == "LiteralString" ? Cell::LiteralString : Cell::Formula;

            OwnPtr<Cell> cell;
            switch (kind) {
            case Cell::LiteralString:
                cell = make<Cell>(obj.get("value").to_string(), position, *sheet);
                break;
            case Cell::Formula: {
                auto& interpreter = sheet->interpreter();
                auto value = interpreter.vm().call(parse_function, json, JS::js_string(interpreter.heap(), obj.get("value").as_string()));
                cell = make<Cell>(obj.get("source").to_string(), move(value), position, *sheet);
                break;
            }
            }

            auto type_name = obj.has("type") ? obj.get("type").to_string() : "Numeric";
            cell->set_type(type_name);

            auto type_meta = obj.get("type_metadata");
            if (type_meta.is_object()) {
                auto& meta_obj = type_meta.as_object();
                auto meta = cell->type_metadata();
                if (auto value = meta_obj.get("length"); value.is_number())
                    meta.length = value.to_i32();
                if (auto value = meta_obj.get("format"); value.is_string())
                    meta.format = value.as_string();
                read_format(meta.static_format, meta_obj);

                cell->set_type_metadata(move(meta));
            }

            auto conditional_formats = obj.get("conditional_formats");
            auto cformats = cell->conditional_formats();
            if (conditional_formats.is_array()) {
                conditional_formats.as_array().for_each([&](const auto& fmt_val) {
                    if (!fmt_val.is_object())
                        return IterationDecision::Continue;

                    auto& fmt_obj = fmt_val.as_object();
                    auto fmt_cond = fmt_obj.get("condition").to_string();
                    if (fmt_cond.is_empty())
                        return IterationDecision::Continue;

                    ConditionalFormat fmt;
                    fmt.condition = move(fmt_cond);
                    read_format(fmt, fmt_obj);
                    cformats.append(move(fmt));

                    return IterationDecision::Continue;
                });
                cell->set_conditional_formats(move(cformats));
            }

            auto evaluated_format = obj.get("evaluated_formats");
            if (evaluated_format.is_object()) {
                auto& evaluated_format_obj = evaluated_format.as_object();
                auto& evaluated_fmts = cell->evaluated_formats();

                read_format(evaluated_fmts, evaluated_format_obj);
            }

            sheet->m_cells.set(position, cell.release_nonnull());
            return IterationDecision::Continue;
        });
    }

    return sheet;
}

Position Sheet::written_data_bounds() const
{
    Position bound;
    for (auto& entry : m_cells) {
        if (entry.value->data().is_empty())
            continue;
        if (entry.key.row >= bound.row)
            bound.row = entry.key.row;
        if (entry.key.column >= bound.column)
            bound.column = entry.key.column;
    }

    return bound;
}

/// The sheet is allowed to have nonstandard column names
/// this checks whether all existing columns are 'standard'
/// (i.e. as generated by 'String::bijective_base_from()'
bool Sheet::columns_are_standard() const
{
    for (size_t i = 0; i < m_columns.size(); ++i) {
        if (m_columns[i] != String::bijective_base_from(i))
            return false;
    }

    return true;
}

JsonObject Sheet::to_json() const
{
    JsonObject object;
    object.set("name", m_name);

    auto save_format = [](const auto& format, auto& obj) {
        if (format.foreground_color.has_value())
            obj.set("foreground_color", format.foreground_color.value().to_string());
        if (format.background_color.has_value())
            obj.set("background_color", format.background_color.value().to_string());
    };

    auto bottom_right = written_data_bounds();

    if (!columns_are_standard()) {
        auto columns = JsonArray();
        for (auto& column : m_columns)
            columns.append(column);
        object.set("columns", move(columns));
    }
    object.set("rows", bottom_right.row + 1);

    JsonObject cells;
    for (auto& it : m_cells) {
        StringBuilder builder;
        builder.append(column(it.key.column));
        builder.appendff("{}", it.key.row);
        auto key = builder.to_string();

        JsonObject data;
        data.set("kind", it.value->kind() == Cell::Kind::Formula ? "Formula" : "LiteralString");
        if (it.value->kind() == Cell::Formula) {
            data.set("source", it.value->data());
            auto json = interpreter().global_object().get("JSON");
            auto stringified = interpreter().vm().call(json.as_object().get("stringify").as_function(), json, it.value->evaluated_data());
            data.set("value", stringified.to_string_without_side_effects());
        } else {
            data.set("value", it.value->data());
        }

        // Set type & meta
        auto& type = it.value->type();
        auto& meta = it.value->type_metadata();
        data.set("type", type.name());

        JsonObject metadata_object;
        metadata_object.set("length", meta.length);
        metadata_object.set("format", meta.format);
#if 0
        metadata_object.set("alignment", alignment_to_string(meta.alignment));
#endif
        save_format(meta.static_format, metadata_object);

        data.set("type_metadata", move(metadata_object));

        // Set conditional formats
        JsonArray conditional_formats;
        for (auto& fmt : it.value->conditional_formats()) {
            JsonObject fmt_object;
            fmt_object.set("condition", fmt.condition);
            save_format(fmt, fmt_object);

            conditional_formats.append(move(fmt_object));
        }

        data.set("conditional_formats", move(conditional_formats));

        auto& evaluated_formats = it.value->evaluated_formats();
        JsonObject evaluated_formats_obj;

        save_format(evaluated_formats, evaluated_formats_obj);
        data.set("evaluated_formats", move(evaluated_formats_obj));

        cells.set(key, move(data));
    }
    object.set("cells", move(cells));

    return object;
}

Vector<Vector<String>> Sheet::to_xsv() const
{
    Vector<Vector<String>> data;

    auto bottom_right = written_data_bounds();

    // First row = headers.
    size_t column_count = m_columns.size();
    if (columns_are_standard()) {
        column_count = bottom_right.column + 1;
        Vector<String> cols;
        for (size_t i = 0; i < column_count; ++i)
            cols.append(m_columns[i]);
        data.append(move(cols));
    } else {
        data.append(m_columns);
    }

    for (size_t i = 0; i <= bottom_right.row; ++i) {
        Vector<String> row;
        row.resize(column_count);
        for (size_t j = 0; j < column_count; ++j) {
            auto cell = at({ j, i });
            if (cell)
                row[j] = cell->typed_display();
        }

        data.append(move(row));
    }

    return data;
}

RefPtr<Sheet> Sheet::from_xsv(const Reader::XSV& xsv, Workbook& workbook)
{
    auto cols = xsv.headers();
    auto rows = xsv.size();

    auto sheet = adopt_ref(*new Sheet(workbook));
    if (xsv.has_explicit_headers()) {
        sheet->m_columns = cols;
    } else {
        sheet->m_columns.ensure_capacity(cols.size());
        for (size_t i = 0; i < cols.size(); ++i)
            sheet->m_columns.append(String::bijective_base_from(i));
    }
    for (size_t i = 0; i < max(rows, Sheet::default_row_count); ++i)
        sheet->add_row();
    if (sheet->columns_are_standard()) {
        for (size_t i = sheet->m_columns.size(); i < Sheet::default_column_count; ++i)
            sheet->add_column();
    }

    for (auto row : xsv) {
        for (size_t i = 0; i < cols.size(); ++i) {
            auto str = row[i];
            if (str.is_empty())
                continue;
            Position position { i, row.index() };
            auto cell = make<Cell>(str, position, *sheet);
            sheet->m_cells.set(position, move(cell));
        }
    }

    return sheet;
}

JsonObject Sheet::gather_documentation() const
{
    JsonObject object;
    const JS::PropertyName doc_name { "__documentation" };

    auto add_docs_from = [&](auto& it, auto& global_object) {
        auto value = global_object.get(it.key);
        if (!value.is_function() && !value.is_object())
            return;

        auto& value_object = value.is_object() ? value.as_object() : value.as_function();
        if (!value_object.has_own_property(doc_name))
            return;

        dbgln("Found '{}'", it.key.to_display_string());
        auto doc = value_object.get(doc_name);
        if (!doc.is_string())
            return;

        JsonParser parser(doc.to_string_without_side_effects());
        auto doc_object = parser.parse();

        if (doc_object.has_value())
            object.set(it.key.to_display_string(), doc_object.value());
        else
            dbgln("Sheet::gather_documentation(): Failed to parse the documentation for '{}'!", it.key.to_display_string());
    };

    for (auto& it : interpreter().global_object().shape().property_table())
        add_docs_from(it, interpreter().global_object());

    for (auto& it : global_object().shape().property_table())
        add_docs_from(it, global_object());

    m_cached_documentation = move(object);
    return m_cached_documentation.value();
}

String Sheet::generate_inline_documentation_for(StringView function, size_t argument_index)
{
    if (!m_cached_documentation.has_value())
        gather_documentation();

    auto& docs = m_cached_documentation.value();
    auto entry = docs.get(function);
    if (entry.is_null() || !entry.is_object())
        return String::formatted("{}(...???{})", function, argument_index);

    auto& entry_object = entry.as_object();
    size_t argc = entry_object.get("argc").to_int(0);
    auto argnames_value = entry_object.get("argnames");
    if (!argnames_value.is_array())
        return String::formatted("{}(...{}???{})", function, argc, argument_index);
    auto& argnames = argnames_value.as_array();
    StringBuilder builder;
    builder.appendff("{}(", function);
    for (size_t i = 0; i < (size_t)argnames.size(); ++i) {
        if (i != 0 && i < (size_t)argnames.size())
            builder.append(", ");
        if (i == argument_index)
            builder.append('<');
        else if (i >= argc)
            builder.append('[');
        builder.append(argnames[i].to_string());
        if (i == argument_index)
            builder.append('>');
        else if (i >= argc)
            builder.append(']');
    }

    builder.append(')');
    return builder.build();
}

String Position::to_cell_identifier(const Sheet& sheet) const
{
    return String::formatted("{}{}", sheet.column(column), row);
}

URL Position::to_url(const Sheet& sheet) const
{
    URL url;
    url.set_protocol("spreadsheet");
    url.set_host("cell");
    url.set_paths({ String::number(getpid()) });
    url.set_fragment(to_cell_identifier(sheet));
    return url;
}

}
