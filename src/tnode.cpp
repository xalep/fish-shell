#include "tnode.h"

const parse_node_t *parse_node_tree_t::next_node_in_node_list(
    const parse_node_t &node_list, parse_token_type_t entry_type,
    const parse_node_t **out_list_tail) const {
    parse_token_type_t list_type = node_list.type;

    // Paranoia - it doesn't make sense for a list type to contain itself.
    assert(list_type != entry_type);

    const parse_node_t *list_cursor = &node_list;
    const parse_node_t *list_entry = NULL;

    // Loop while we don't have an item but do have a list. Note that some nodes may contain
    // nothing; e.g. job_list contains blank lines as a production.
    while (list_entry == NULL && list_cursor != NULL) {
        const parse_node_t *next_cursor = NULL;

        // Walk through the children.
        for (node_offset_t i = 0; i < list_cursor->child_count; i++) {
            const parse_node_t *child = this->get_child(*list_cursor, i);
            if (child->type == entry_type) {
                // This is the list entry.
                list_entry = child;
            } else if (child->type == list_type) {
                // This is the next in the list.
                next_cursor = child;
            }
        }
        // Go to the next entry, even if it's NULL.
        list_cursor = next_cursor;
    }

    // Return what we got.
    assert(list_cursor == NULL || list_cursor->type == list_type);
    assert(list_entry == NULL || list_entry->type == entry_type);
    if (out_list_tail != NULL) *out_list_tail = list_cursor;
    return list_entry;
}

enum parse_statement_decoration_t get_decoration(tnode_t<grammar::plain_statement> stmt) {
    parse_statement_decoration_t decoration = parse_statement_decoration_none;
    if (auto decorated_statement = stmt.try_get_parent<grammar::decorated_statement>()) {
        decoration = static_cast<parse_statement_decoration_t>(decorated_statement.tag());
    }
    return decoration;
}

enum parse_bool_statement_type_t bool_statement_type(tnode_t<grammar::boolean_statement> stmt) {
    return static_cast<parse_bool_statement_type_t>(stmt.tag());
}

enum token_type redirection_type(tnode_t<grammar::redirection> redirection, const wcstring &src,
                                 int *out_fd, wcstring *out_target) {
    assert(redirection && "redirection is missing");
    enum token_type result = TOK_NONE;
    tnode_t<grammar::tok_redirection> prim = redirection.child<0>();  // like 2>
    assert(prim && "expected to have primitive");

    if (prim.has_source()) {
        result = redirection_type_for_string(prim.get_source(src), out_fd);
    }
    if (out_target != NULL) {
        tnode_t<grammar::tok_string> target = redirection.child<1>();  // like &1 or file path
        *out_target = target.has_source() ? target.get_source(src) : wcstring();
    }
    return result;
}

std::vector<tnode_t<grammar::comment>> parse_node_tree_t::comment_nodes_for_node(
    const parse_node_t &parent) const {
    std::vector<tnode_t<grammar::comment>> result;
    if (parent.has_comments()) {
        // Walk all our nodes, looking for comment nodes that have the given node as a parent.
        for (size_t i = 0; i < this->size(); i++) {
            const parse_node_t &potential_comment = this->at(i);
            if (potential_comment.type == parse_special_type_comment &&
                this->get_parent(potential_comment) == &parent) {
                result.emplace_back(this, &potential_comment);
            }
        }
    }
    return result;
}

maybe_t<wcstring> command_for_plain_statement(tnode_t<grammar::plain_statement> stmt,
                                              const wcstring &src) {
    tnode_t<grammar::tok_string> cmd = stmt.child<0>();
    if (cmd && cmd.has_source()) {
        return cmd.get_source(src);
    }
    return none();
}

arguments_node_list_t get_argument_nodes(tnode_t<grammar::argument_list> list, size_t max) {
    return list.descendants<grammar::argument>(max);
}

arguments_node_list_t get_argument_nodes(tnode_t<grammar::arguments_or_redirections_list> list,
                                         size_t max) {
    return list.descendants<grammar::argument>(max);
}

bool job_node_is_background(tnode_t<grammar::job> job) {
    tnode_t<grammar::optional_background> bg = job.child<2>();
    return bg.tag() == parse_background;
}

bool statement_is_in_pipeline(tnode_t<grammar::statement> st, bool include_first) {
    using namespace grammar;
    if (!st) {
        return false;
    }

    // If we're part of a job continuation, we're definitely in a pipeline.
    if (st.try_get_parent<job_continuation>()) {
        return true;
    }

    // If include_first is set, check if we're the beginning of a job, and if so, whether that job
    // has a non-empty continuation.
    if (include_first) {
        tnode_t<job_continuation> jc = st.try_get_parent<job>().child<1>();
        if (jc.try_get_child<statement, 2>()) {
            return true;
        }
    }
    return false;
}
