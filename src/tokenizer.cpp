// A specialized tokenizer for tokenizing the fish language. In the future, the tokenizer should be
// extended to support marks, tokenizing multiple strings and disposing of unused string segments.
#include "config.h"  // IWYU pragma: keep

#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <string>
#include <type_traits>

#include "common.h"
#include "fallback.h"  // IWYU pragma: keep
#include "tokenizer.h"
#include "wutil.h"  // IWYU pragma: keep

/// Error string for unexpected end of string.
#define QUOTE_ERROR _(L"Unexpected end of string, quotes are not balanced")

/// Error string for mismatched parenthesis.
#define PARAN_ERROR _(L"Unexpected end of string, parenthesis do not match")

/// Error string for mismatched square brackets.
#define SQUARE_BRACKET_ERROR _(L"Unexpected end of string, square brackets do not match")

/// Error string for unterminated escape (backslash without continuation).
#define UNTERMINATED_ESCAPE_ERROR _(L"Unexpected end of string, incomplete escape sequence")

/// Error string for invalid redirections.
#define REDIRECT_ERROR _(L"Invalid input/output redirection")

/// Error string for when trying to pipe from fd 0.
#define PIPE_ERROR _(L"Cannot use stdin (fd 0) as pipe output")

/// Set the latest tokens string to be the specified error message.
void tokenizer_t::call_error(enum tokenizer_error error_type, const wchar_t *where) {
    assert(error_type != TOK_ERROR_NONE && "TOK_ERROR_NONE passed to call_error");
    this->last_type = TOK_ERROR;
    this->error = error_type;
    this->has_next = false;
    this->global_error_offset = where ? where - this->start : 0;
    if (this->squash_errors) {
        this->last_token.clear();
    } else {
        switch (error_type) {
            case TOK_UNTERMINATED_QUOTE:
                this->last_token = QUOTE_ERROR;
                break;
            case TOK_UNTERMINATED_SUBSHELL:
                this->last_token = PARAN_ERROR;
                break;
            case TOK_UNTERMINATED_SLICE:
                this->last_token = SQUARE_BRACKET_ERROR;
                break;
            case TOK_UNTERMINATED_ESCAPE:
                this->last_token = UNTERMINATED_ESCAPE_ERROR;
                break;
            case TOK_INVALID_REDIRECT:
                this->last_token = REDIRECT_ERROR;
                break;
            case TOK_INVALID_PIPE:
                this->last_token = PIPE_ERROR;
                break;
            default:
                assert(0 && "Unknown error type");
        }
    }
}

tokenizer_t::tokenizer_t(const wchar_t *start, tok_flags_t flags) : buff(start), start(start) {
    assert(start != nullptr && "Invalid start");

    this->accept_unfinished = static_cast<bool>(flags & TOK_ACCEPT_UNFINISHED);
    this->show_comments = static_cast<bool>(flags & TOK_SHOW_COMMENTS);
    this->squash_errors = static_cast<bool>(flags & TOK_SQUASH_ERRORS);
    this->show_blank_lines = static_cast<bool>(flags & TOK_SHOW_BLANK_LINES);
}

bool tokenizer_t::next(struct tok_t *result) {
    assert(result != NULL);
    if (!this->tok_next()) {
        return false;
    }

    const size_t current_pos = this->buff - this->start;

    // We want to copy our last_token into result->text. If we just do this naively via =, we are
    // liable to trigger std::string's CoW implementation: result->text's storage will be
    // deallocated and instead will acquire a reference to last_token's storage. But last_token will
    // be overwritten soon, which will trigger a new allocation and a copy. So our attempt to re-use
    // result->text's storage will have failed. To ensure that doesn't happen, use assign() with
    // wchar_t.
    result->text.assign(this->last_token.data(), this->last_token.size());

    result->type = this->last_type;
    result->offset = this->last_pos;
    result->error = this->last_type == TOK_ERROR ? this->error : TOK_ERROR_NONE;
    assert(this->buff >= this->start);

    // Compute error offset.
    result->error_offset = 0;
    if (this->last_type == TOK_ERROR && this->global_error_offset >= this->last_pos &&
        this->global_error_offset < current_pos) {
        result->error_offset = this->global_error_offset - this->last_pos;
    }

    assert(this->buff >= this->start);
    result->length = current_pos >= this->last_pos ? current_pos - this->last_pos : 0;
    return true;
}

/// Tests if this character can be a part of a string. The redirect ^ is allowed unless it's the
/// first character. Hash (#) starts a comment if it's the first character in a token; otherwise it
/// is considered a string character. See issue #953.
static bool tok_is_string_character(wchar_t c, bool is_first) {
    switch (c) {
        case L'\0':
        case L' ':
        case L'\n':
        case L'|':
        case L'\t':
        case L';':
        case L'\r':
        case L'<':
        case L'>':
        case L'&': {
            // Unconditional separators.
            return false;
        }
        case L'^': {
            // Conditional separator.
            return !is_first;
        }
        default: { return true; }
    }
}

/// Quick test to catch the most common 'non-magical' characters, makes read_string slightly faster
/// by adding a fast path for the most common characters. This is obviously not a suitable
/// replacement for iswalpha.
static int myal(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

/// Read the next token as a string.
void tokenizer_t::read_string() {
    long len;
    int do_loop = 1;
    size_t paran_count = 0;
    // Up to 96 open parens, before we give up on good error reporting.
    const size_t paran_offsets_max = 96;
    size_t paran_offsets[paran_offsets_max];
    // Where the open bracket is.
    size_t offset_of_bracket = 0;
    const wchar_t *const buff_start = this->buff;
    bool is_first = true;

    enum tok_mode_t {
        mode_regular_text = 0,    // regular text
        mode_subshell = 1,        // inside of subshell
        mode_array_brackets = 2,  // inside of array brackets
        mode_array_brackets_and_subshell =
            3  // inside of array brackets and subshell, like in '$foo[(ech'
    } mode = mode_regular_text;

    while (1) {
        if (!myal(*this->buff)) {
            if (*this->buff == L'\\') {
                const wchar_t *error_location = this->buff;
                this->buff++;
                if (*this->buff == L'\0') {
                    if ((!this->accept_unfinished)) {
                        this->call_error(TOK_UNTERMINATED_ESCAPE, error_location);
                        return;
                    }
                    // Since we are about to increment tok->buff, decrement it first so the
                    // increment doesn't go past the end of the buffer. See issue #389.
                    this->buff--;
                    do_loop = 0;
                }

                this->buff++;
                continue;
            }

            switch (mode) {
                case mode_regular_text: {
                    switch (*this->buff) {
                        case L'(': {
                            paran_count = 1;
                            paran_offsets[0] = this->buff - this->start;
                            mode = mode_subshell;
                            break;
                        }
                        case L'[': {
                            if (this->buff != buff_start) {
                                mode = mode_array_brackets;
                                offset_of_bracket = this->buff - this->start;
                            }
                            break;
                        }
                        case L'\'':
                        case L'"': {
                            const wchar_t *end = quote_end(this->buff);
                            if (end) {
                                this->buff = end;
                            } else {
                                const wchar_t *error_loc = this->buff;
                                this->buff += wcslen(this->buff);

                                if (!this->accept_unfinished) {
                                    this->call_error(TOK_UNTERMINATED_QUOTE, error_loc);
                                    return;
                                }
                                do_loop = 0;
                            }
                            break;
                        }
                        default: {
                            if (!tok_is_string_character(*(this->buff), is_first)) {
                                do_loop = 0;
                            }
                            break;
                        }
                    }
                    break;
                }

                case mode_array_brackets_and_subshell:
                case mode_subshell: {
                    switch (*this->buff) {
                        case L'\'':
                        case L'\"': {
                            const wchar_t *end = quote_end(this->buff);
                            if (end) {
                                this->buff = end;
                            } else {
                                const wchar_t *error_loc = this->buff;
                                this->buff += wcslen(this->buff);
                                if ((!this->accept_unfinished)) {
                                    this->call_error(TOK_UNTERMINATED_QUOTE, error_loc);
                                    return;
                                }
                                do_loop = 0;
                            }
                            break;
                        }
                        case L'(': {
                            if (paran_count < paran_offsets_max) {
                                paran_offsets[paran_count] = this->buff - this->start;
                            }
                            paran_count++;
                            break;
                        }
                        case L')': {
                            assert(paran_count > 0);
                            paran_count--;
                            if (paran_count == 0) {
                                mode =
                                    (mode == mode_array_brackets_and_subshell ? mode_array_brackets
                                                                              : mode_regular_text);
                            }
                            break;
                        }
                        case L'\0': {
                            do_loop = 0;
                            break;
                        }
                        default: {
                            break;  // ignore other chars
                        }
                    }
                    break;
                }

                case mode_array_brackets: {
                    switch (*this->buff) {
                        case L'(': {
                            paran_count = 1;
                            paran_offsets[0] = this->buff - this->start;
                            mode = mode_array_brackets_and_subshell;
                            break;
                        }
                        case L']': {
                            mode = mode_regular_text;
                            break;
                        }
                        case L'\0': {
                            do_loop = 0;
                            break;
                        }
                        default: {
                            break;  // ignore other chars
                        }
                    }
                    break;
                }
            }
        }

        if (!do_loop) break;

        this->buff++;
        is_first = false;
    }

    if ((!this->accept_unfinished) && (mode != mode_regular_text)) {
        switch (mode) {
            case mode_subshell: {
                // Determine the innermost opening paran offset by interrogating paran_offsets.
                assert(paran_count > 0);
                size_t offset_of_open_paran = 0;
                if (paran_count <= paran_offsets_max) {
                    offset_of_open_paran = paran_offsets[paran_count - 1];
                }

                this->call_error(TOK_UNTERMINATED_SUBSHELL, this->start + offset_of_open_paran);
                break;
            }
            case mode_array_brackets:
            case mode_array_brackets_and_subshell: {
                this->call_error(TOK_UNTERMINATED_SLICE, this->start + offset_of_bracket);
                break;
            }
            default: {
                DIE("unexpected mode in read_string");
                break;
            }
        }
        return;
    }

    len = this->buff - buff_start;

    this->last_token.assign(buff_start, len);
    this->last_type = TOK_STRING;
}

/// Reads a redirection or an "fd pipe" (like 2>|) from a string. Returns how many characters were
/// consumed. If zero, then this string was not a redirection. Also returns by reference the
/// redirection mode, and the fd to redirection. If there is overflow, *out_fd is set to -1.
static size_t read_redirection_or_fd_pipe(const wchar_t *buff,
                                          enum token_type *out_redirection_mode, int *out_fd) {
    bool errored = false;
    int fd = 0;
    enum token_type redirection_mode = TOK_NONE;

    size_t idx = 0;

    // Determine the fd. This may be specified as a prefix like '2>...' or it may be implicit like
    // '>' or '^'. Try parsing out a number; if we did not get any digits then infer it from the
    // first character. Watch out for overflow.
    long long big_fd = 0;
    for (; iswdigit(buff[idx]); idx++) {
        // Note that it's important we consume all the digits here, even if it overflows.
        if (big_fd <= INT_MAX) big_fd = big_fd * 10 + (buff[idx] - L'0');
    }

    fd = (big_fd > INT_MAX ? -1 : static_cast<int>(big_fd));

    if (idx == 0) {
        // We did not find a leading digit, so there's no explicit fd. Infer it from the type.
        switch (buff[idx]) {
            case L'>': {
                fd = STDOUT_FILENO;
                break;
            }
            case L'<': {
                fd = STDIN_FILENO;
                break;
            }
            case L'^': {
                fd = STDERR_FILENO;
                break;
            }
            default: {
                errored = true;
                break;
            }
        }
    }

    // Either way we should have ended on the redirection character itself like '>'.
    // Don't allow an fd with a caret redirection - see #1873
    wchar_t redirect_char = buff[idx++];  // note increment of idx
    if (redirect_char == L'>' || (redirect_char == L'^' && idx == 1)) {
        redirection_mode = TOK_REDIRECT_OUT;
        if (buff[idx] == redirect_char) {
            // Doubled up like ^^ or >>. That means append.
            redirection_mode = TOK_REDIRECT_APPEND;
            idx++;
        }
    } else if (redirect_char == L'<') {
        redirection_mode = TOK_REDIRECT_IN;
    } else {
        // Something else.
        errored = true;
    }

    // Don't return valid-looking stuff on error.
    if (errored) {
        idx = 0;
        redirection_mode = TOK_NONE;
    } else {
        // Optional characters like & or ?, or the pipe char |.
        wchar_t opt_char = buff[idx];
        if (opt_char == L'&') {
            redirection_mode = TOK_REDIRECT_FD;
            idx++;
        } else if (opt_char == L'?') {
            redirection_mode = TOK_REDIRECT_NOCLOB;
            idx++;
        } else if (opt_char == L'|') {
            // So the string looked like '2>|'. This is not a redirection - it's a pipe! That gets
            // handled elsewhere.
            redirection_mode = TOK_PIPE;
            idx++;
        }
    }

    // Return stuff.
    if (out_redirection_mode != NULL) *out_redirection_mode = redirection_mode;
    if (out_fd != NULL) *out_fd = fd;

    return idx;
}

enum token_type redirection_type_for_string(const wcstring &str, int *out_fd) {
    enum token_type mode = TOK_NONE;
    int fd = 0;
    read_redirection_or_fd_pipe(str.c_str(), &mode, &fd);
    // Redirections only, no pipes.
    if (mode == TOK_PIPE || fd < 0) mode = TOK_NONE;
    if (out_fd != NULL) *out_fd = fd;
    return mode;
}

int fd_redirected_by_pipe(const wcstring &str) {
    // Hack for the common case.
    if (str == L"|") {
        return STDOUT_FILENO;
    }

    enum token_type mode = TOK_NONE;
    int fd = 0;
    read_redirection_or_fd_pipe(str.c_str(), &mode, &fd);
    // Pipes only.
    if (mode != TOK_PIPE || fd < 0) fd = -1;
    return fd;
}

int oflags_for_redirection_type(enum token_type type) {
    switch (type) {
        case TOK_REDIRECT_APPEND: {
            return O_CREAT | O_APPEND | O_WRONLY;
        }
        case TOK_REDIRECT_OUT: {
            return O_CREAT | O_WRONLY | O_TRUNC;
        }
        case TOK_REDIRECT_NOCLOB: {
            return O_CREAT | O_EXCL | O_WRONLY;
        }
        case TOK_REDIRECT_IN: {
            return O_RDONLY;
        }
        default: { return -1; }
    }
}

/// Test if a character is whitespace. Differs from iswspace in that it does not consider a newline
/// to be whitespace.
static bool iswspace_not_nl(wchar_t c) {
    switch (c) {
        case L' ':
        case L'\t':
        case L'\r':
            return true;
        case L'\n':
            return false;
        default:
            return iswspace(c);
    }
}

bool tokenizer_t::tok_next() {
    if (!this->has_next) {
        return false;
    }

    // Consume non-newline whitespace. If we get an escaped newline, mark it and continue past it.
    for (;;) {
        if (this->buff[0] == L'\\' && this->buff[1] == L'\n') {
            this->buff += 2;
            this->continue_line_after_comment = true;
        } else if (iswspace_not_nl(this->buff[0])) {
            this->buff++;
        } else {
            break;
        }
    }

    while (*this->buff == L'#') {
        // We have a comment, walk over the comment.
        const wchar_t *comment_start = this->buff;
        while (this->buff[0] != L'\n' && this->buff[0] != L'\0') this->buff++;
        size_t comment_len = this->buff - comment_start;

        // If we are going to continue after the comment, skip any trailing newline.
        if (this->buff[0] == L'\n' && this->continue_line_after_comment) this->buff++;

        // Maybe return the comment.
        if (this->show_comments) {
            this->last_pos = comment_start - this->start;
            this->last_token.assign(comment_start, comment_len);
            this->last_type = TOK_COMMENT;
            return true;
        }
        while (iswspace_not_nl(this->buff[0])) this->buff++;
    }

    // We made it past the comments and ate any trailing newlines we wanted to ignore.
    this->continue_line_after_comment = false;
    this->last_pos = this->buff - this->start;

    switch (*this->buff) {
        case L'\0': {
            this->last_type = TOK_END;
            this->has_next = false;
            this->last_token.clear();
            return false;
        }
        case L'\r':  // carriage-return
        case L'\n':  // newline
        case L';': {
            this->last_type = TOK_END;
            this->last_token.assign(1, *this->buff);
            this->buff++;
            // Hack: when we get a newline, swallow as many as we can. This compresses multiple
            // subsequent newlines into a single one.
            if (!this->show_blank_lines) {
                while (*this->buff == L'\n' || *this->buff == 13 /* CR */ || *this->buff == ' ' ||
                       *this->buff == '\t') {
                    this->buff++;
                }
            }
            break;
        }
        case L'&': {
            this->last_type = TOK_BACKGROUND;
            this->buff++;
            break;
        }
        case L'|': {
            this->last_token = L"1";
            this->last_type = TOK_PIPE;
            this->buff++;
            break;
        }
        case L'>':
        case L'<':
        case L'^': {
            // There's some duplication with the code in the default case below. The key difference
            // here is that we must never parse these as a string; a failed redirection is an error!
            enum token_type mode = TOK_NONE;
            int fd = -1;
            size_t consumed = read_redirection_or_fd_pipe(this->buff, &mode, &fd);
            if (consumed == 0 || fd < 0) {
                this->call_error(TOK_INVALID_REDIRECT, this->buff);
            } else {
                this->buff += consumed;
                this->last_type = mode;
                this->last_token = to_string(fd);
            }
            break;
        }
        default: {
            // Maybe a redirection like '2>&1', maybe a pipe like 2>|, maybe just a string.
            const wchar_t *error_location = this->buff;
            size_t consumed = 0;
            enum token_type mode = TOK_NONE;
            int fd = -1;
            if (iswdigit(*this->buff)) {
                consumed = read_redirection_or_fd_pipe(this->buff, &mode, &fd);
            }

            if (consumed > 0) {
                // It looks like a redirection or a pipe. But we don't support piping fd 0. Note
                // that fd 0 may be -1, indicating overflow; but we don't treat that as a tokenizer
                // error.
                if (mode == TOK_PIPE && fd == 0) {
                    this->call_error(TOK_INVALID_PIPE, error_location);
                } else {
                    this->buff += consumed;
                    this->last_type = mode;
                    this->last_token = to_string(fd);
                }
            } else {
                // Not a redirection or pipe, so just a string.
                this->read_string();
            }
            break;
        }
    }
    return true;
}

wcstring tok_first(const wcstring &str) {
    wcstring result;
    tokenizer_t t(str.data(), TOK_SQUASH_ERRORS);
    tok_t token;
    if (t.next(&token) && token.type == TOK_STRING) {
        result = std::move(token.text);
    }
    return result;
}

bool move_word_state_machine_t::consume_char_punctuation(wchar_t c) {
    enum { s_always_one = 0, s_whitespace, s_alphanumeric, s_end };

    bool consumed = false;
    while (state != s_end && !consumed) {
        switch (state) {
            case s_always_one: {
                // Always consume the first character.
                consumed = true;
                state = s_whitespace;
                break;
            }
            case s_whitespace: {
                if (iswspace(c)) {
                    // Consumed whitespace.
                    consumed = true;
                } else {
                    state = s_alphanumeric;
                }
                break;
            }
            case s_alphanumeric: {
                if (iswalnum(c)) {
                    consumed = true;  // consumed alphanumeric
                } else {
                    state = s_end;
                }
                break;
            }
            case s_end:
            default: { break; }
        }
    }
    return consumed;
}

bool move_word_state_machine_t::is_path_component_character(wchar_t c) {
    // Always treat separators as first. All this does is ensure that we treat ^ as a string
    // character instead of as stderr redirection, which I hypothesize is usually what is desired.
    return tok_is_string_character(c, true) && !wcschr(L"/={,}'\"", c);
}

bool move_word_state_machine_t::consume_char_path_components(wchar_t c) {
    enum {
        s_initial_punctuation,
        s_whitespace,
        s_separator,
        s_slash,
        s_path_component_characters,
        s_end
    };

    // fwprintf(stdout, L"state %d, consume '%lc'\n", state, c);
    bool consumed = false;
    while (state != s_end && !consumed) {
        switch (state) {
            case s_initial_punctuation: {
                if (!is_path_component_character(c)) {
                    consumed = true;
                }
                state = s_whitespace;
                break;
            }
            case s_whitespace: {
                if (iswspace(c)) {
                    consumed = true;  // consumed whitespace
                } else if (c == L'/' || is_path_component_character(c)) {
                    state = s_slash;  // path component
                } else {
                    state = s_separator;  // path separator
                }
                break;
            }
            case s_separator: {
                if (!iswspace(c) && !is_path_component_character(c)) {
                    consumed = true;  // consumed separator
                } else {
                    state = s_end;
                }
                break;
            }
            case s_slash: {
                if (c == L'/') {
                    consumed = true;  // consumed slash
                } else {
                    state = s_path_component_characters;
                }
                break;
            }
            case s_path_component_characters: {
                if (is_path_component_character(c)) {
                    consumed = true;  // consumed string character except slash
                } else {
                    state = s_end;
                }
                break;
            }
            case s_end:
            default: { break; }
        }
    }
    return consumed;
}

bool move_word_state_machine_t::consume_char_whitespace(wchar_t c) {
    enum { s_always_one = 0, s_blank, s_graph, s_end };

    bool consumed = false;
    while (state != s_end && !consumed) {
        switch (state) {
            case s_always_one: {
                consumed = true;  // always consume the first character
                state = s_blank;
                break;
            }
            case s_blank: {
                if (iswblank(c)) {
                    consumed = true;  // consumed whitespace
                } else {
                    state = s_graph;
                }
                break;
            }
            case s_graph: {
                if (iswgraph(c)) {
                    consumed = true;  // consumed printable non-space
                } else {
                    state = s_end;
                }
                break;
            }
            case s_end:
            default: { break; }
        }
    }
    return consumed;
}

bool move_word_state_machine_t::consume_char(wchar_t c) {
    switch (style) {
        case move_word_style_punctuation: {
            return consume_char_punctuation(c);
        }
        case move_word_style_path_components: {
            return consume_char_path_components(c);
        }
        case move_word_style_whitespace: {
            return consume_char_whitespace(c);
        }
    }

    DIE("should not reach this statement");  // silence some compiler errors about not returning
}

move_word_state_machine_t::move_word_state_machine_t(move_word_style_t syl)
    : state(0), style(syl) {}

void move_word_state_machine_t::reset() { state = 0; }
