
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *debug;

//////////////////////////////////////////
// Data structures

typedef unsigned long long u64;

typedef struct {
	int len;
	char *start;
} substr;

void fputsubstr(substr substr, FILE *out) {
	fwrite(substr.start, 1, substr.len, out);
}

typedef struct {
	unsigned length; // number of chars used
	unsigned capacity; // number of POINTERS allocated
	char **data;
} Builder;

bool min_u32(unsigned x, unsigned y) {
	return x < y ? y : x;
}

char *builder_get_ptr(Builder *builder, unsigned i) {
	return &builder->data[i / 1024][i % 1024];
}

void builder_log(Builder *data) {
	fputs("proc = ", debug);
	for (int j = 0; j < data->length; j++) {
		fprintf(debug, " %u", (unsigned char) *builder_get_ptr(data, j));
	}
	fputs("\n", debug);
}

void builder_append(Builder *builder, unsigned data_len, char *data) {
	unsigned i = 0;
	while (i < data_len) {
		unsigned x = builder->length / 1024, y = builder->length % 1024;
		if (y == 0) {
			if (x == builder->capacity) {
				unsigned new_capacity = builder->capacity ? 2 * builder->capacity : 1;
				char** new_data = (char**)malloc(new_capacity * sizeof (char*));
				memcpy(new_data, builder->data, builder->capacity * sizeof (char*));
				if (builder->data) free(builder->data);
				builder->data = new_data;
				builder->capacity = new_capacity;
			}
			builder->data[x] = (char*)malloc(1024);
		}
		unsigned diff = min_u32(1024 - y, data_len - i);
		memcpy(builder->data[x] + y, data + i, diff);
		i += diff;
		builder->length += diff;
	}
}

#define builder_push_generic(name, T) void name (Builder *builder, T data) { builder_append(builder, sizeof ( T ) , (char*)&data); }

builder_push_generic(builder_push_u64, u64)
builder_push_generic(builder_push_u8, char)
builder_push_generic(builder_push_substr, substr)

void builder_read(Builder *builder, unsigned start, unsigned size, char *out) {
	unsigned end = start + size;
	unsigned i = 0;
	if (end > builder->length) {
		printf("Tried to read past end of builder!\n");
		exit(-1);
	}
	while (start + i < end) {
		unsigned x = (start + i) / 1024, y = (start + i) % 1024;
		unsigned diff = min_u32(1024 - y, size - i);
		memcpy(out + i, builder->data[x] + y, diff);
		i += diff;
	}
}

#define builder_read_generic(name, T) T name (Builder *builder, unsigned index) { T result; builder_read(builder, index, sizeof ( T ) , (char*)&result); return result; }

builder_read_generic(builder_read_u64, u64)
builder_read_generic(builder_read_u8, char)
builder_read_generic(builder_read_substr, substr)

//////////////////////////////////////////
// Token List

#define TOKEN_WIDTH 16

typedef struct {
	char str[TOKEN_WIDTH];
	bool is_keyword;
} TokenDef;

typedef enum {
	T_ALPHANUM = -1,
	T_OPEN_BRACE,
	T_OPEN_PAREN,
	T_OPEN_BRACK,
	T_CLOSE_BRACE,
	T_CLOSE_PAREN,
	T_CLOSE_BRACK,
	T_DEFINITION,
	T_COMMA,
	T_SEMICOLON,
	T_COLON,
	T_BIND,
	T_ARROW,
	T_ASTERISK,
	T_PLUS,
	T_RETURN,
	T_B64,
	T_B8,
	T_LOCAL,

	// number of possible tokens, not to be used as a variant...
	UTOKEN_NUM,
} TokenVariant;

#define T_FIRST_OPEN T_OPEN_BRACE
#define T_LAST_OPEN T_OPEN_BRACK

#define T_TOTAL_OPENS (T_LAST_OPEN - T_FIRST_OPEN + 1)

// value TokenDef utokens[UTOKEN_NUM] = 
TokenDef utokens[UTOKEN_NUM] = {
	{"{", false},
	{"(", false},
	{"[", false},
	{"}", false},
	{")", false},
	{"]", false},
	{":=", false},
	{",", false},
	{";", false},
	{":", false},
	{"<-", false},
	{"->", false},
	{"*", false},
	{"+", false},
	{"return", true},
	{"b64", true},
	{"b8", true},
	{"local", true},
};

bool is_alphanum(char c) {
	return
		('0' <= c && c <= '9') ||
		('A' <= c && c <= 'Z') ||
		('a' <= c && c <= 'z') ||
		c == '_';
}

struct TokenBranch;

typedef struct {
	int branch_num;
	struct TokenBranch *branches;
} TokenTree;

typedef struct TokenBranch {
	TokenVariant variant;
	union {
		substr substr;
		TokenTree subtree;
	} data;
} TokenBranch;

//////////////////////////////////////////
// Tokenizer

int prefix_whitespace(const char *input) {
	int result = 0;
	bool done = false;
	while (!done) {
		char c = input[result];
		if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
			result++;
		} else {
			done = true;
		}
	}
	return result;
}

int prefix_token(const char *input, TokenDef *utoken) {
	char *str = utoken->str;
	bool is_keyword = utoken->is_keyword;
	int i = 0;
	while (input[i] != '\0' && str[i] != '\0') {
		if (input[i] == '\0' || input[i] != str[i]) {
			return 0;
		}
		i++;
	}
	if(is_keyword && is_alphanum(input[i])) {
		// keywords must be followed by whitespace or an operator
		// in order to tokenize
		return 0;
	}
	return i;
}

substr next_token(char *input) {
	substr result;
	// default to 1 so that tokenizer always moves forward
	// really we should report an error when we reach an unknown operator
	result.len = 1;
	result.start = input;
	bool valid = true;
	while (valid) {
		char c = input[result.len];
		valid = is_alphanum(c);
		if (valid) {
			result.len++;
		}
	}
	return result;
}

TokenTree new_tree(int max_branches) {
	TokenTree result;
	result.branch_num = 0;
	result.branches = NULL;

	if (max_branches > 0) {
		result.branches =
			(TokenBranch*) malloc(sizeof(TokenBranch) * max_branches);
		for (int i = 0; i < max_branches; i++) {
			// we don't actually need to initialize anything but num
			// we can set num later when we change variant to >0
			// we do this instead to satisfy linters
			TokenBranch* out = &result.branches[i];
			out->variant = -2;
			out->data.subtree.branch_num = 0;
			out->data.subtree.branches = NULL;
		}
	}

	return result;
}

// borrows utokens during function
// borrows input for lifetime of result
TokenTree tokenize_flat(char *input, int input_len) {
	char *end = input + input_len;

	TokenTree result = new_tree(input_len);

	while (input < end) {
		input = input + prefix_whitespace(input);
		TokenVariant variant = T_ALPHANUM;
		for (TokenVariant ti = 0; ti < UTOKEN_NUM; ti++) {
			int len = prefix_token(input, &utokens[ti]);
			if (len) {
				variant = ti;
				input += len;
				break;
			}
		}

		TokenBranch *branch = result.branches + result.branch_num;
		result.branch_num++;

		branch->variant = variant;
		if (variant == T_ALPHANUM) {
			substr substr = next_token(input);
			branch->data.substr = substr;
			input += substr.len;
		} else {
			// so that when we recursively destroy tt, we don't free garbage
			branch->data.subtree.branches = NULL;
		}
	}

	return result;
}

typedef struct {
	TokenTree tt;
	TokenBranch *remaining;
} groupResult;

groupResult group_tokens_starting_from(
	TokenBranch *remaining,
	TokenBranch *end,
	int close
) {
	TokenTree tt = new_tree(end - remaining);
	TokenBranch *out = tt.branches;
	int variant;
	while (remaining < end) {
		variant = remaining->variant;
		if (variant == close) {
			// so that we return one past the close bracket
			remaining++;
			break;
		}
		// TODO: detect bad close braces to prevent "unexpected: }"?
		*out = *remaining;
		// increment before checking open brackets, so that we can recurse
		remaining++;

		if (variant >= T_FIRST_OPEN && variant <= T_LAST_OPEN) {
			groupResult subtree = group_tokens_starting_from(remaining, end,
					variant + T_TOTAL_OPENS);
			out->data.subtree = subtree.tt;
			remaining = subtree.remaining;
		}
		tt.branch_num++;
		out++;
	}
	groupResult result;
	result.tt = tt;
	result.remaining = remaining;
	return result;
}

// reborrows strings from ts, but allocates new branches
TokenTree group_tokens(TokenTree ts) {
	TokenBranch *start = ts.branches;
	TokenBranch *end = start + ts.branch_num;
	groupResult tt = group_tokens_starting_from(start, end, -2);
	return tt.tt;
}

void destroy_tt(TokenTree tt) {
	for (int i = 0; i < tt.branch_num; i++) {
		TokenBranch *branch = &tt.branches[i];
		if (branch->variant != T_ALPHANUM) {
			destroy_tt(branch->data.subtree);
		}
	}
}

///////////////////////////
// parser

typedef enum {
	I_FUN_DEF
} ItemVariant;

typedef struct Type {
	int indirection;
	enum {
		TY_B64,
		TY_B8,
		TY_ARRAY,
		TY_STRUCT,
	} variant;
	unsigned length;
	struct Type *fields;
} Type;

typedef struct {
	substr ident;
	Type type;
} Binding;

typedef enum {
	E_END,
	E_IDENT,
	E_INTEGER_LITERAL,
	E_DEREF,
	E_PLUS,
} ExprVariant;

typedef enum {
	S_EVAL,
	S_LOCAL,
	S_RET,
} StatementVariant;

typedef enum {
	SL_DISCARD = 70,
	SL_DEF,
	SL_WRITE,
} OutputLocation;

typedef struct {
	substr name;
	int param_num;
	Binding *params;
	Type ret_type;
	Builder body;
} Proc;

typedef struct {
	int item_num;
	Proc *items;
} Items;

// @Robustness we might walk past end of array
Type parse_type(TokenBranch **iter) {
	Type result;
	result.indirection = 0;
	while ((*iter)->variant == T_ASTERISK) {
		fputc('*', debug);
		*iter += 1;
		result.indirection += 1;
	}
	result.length = 0;
	if ((*iter)->variant == T_B64) {
		fputs("b64", debug);
		result.variant = TY_B64;
	} else if ((*iter)->variant == T_B8) {
		fputs("b8", debug);
		result.variant = TY_B8;
	} else if ((*iter)->variant == T_OPEN_BRACK) {
		fputc('[', debug);
		result.variant = TY_ARRAY;
		result.length = 1;
		result.fields = malloc(sizeof(Type));
		TokenBranch *start = (*iter)->data.subtree.branches;
		TokenBranch *end = start + (*iter)->data.subtree.branch_num;
		*result.fields = parse_type(&start);
		if (start != end) {
			printf("Expected close bracket after type\n");
			exit(-1);
		}
		fputc(']', debug);
	} else if ((*iter)->variant == T_OPEN_BRACE) {
		fputc('{', debug);
		result.variant = TY_STRUCT;
		int size_guess = (*iter)->data.subtree.branch_num / 2 + 1;
		// @Performance linear allocator would do nicely here
		// or another serialized builder structure? maybe not.
		result.fields = malloc(sizeof(Type)*size_guess);
		bool comma = true;
		TokenBranch *start = (*iter)->data.subtree.branches;
		TokenBranch *end = start + (*iter)->data.subtree.branch_num;
		while (start < end) {
			if (comma) {
				if (start->variant == T_COMMA) {
					printf("Expected type after comma in struct type\n");
					exit(-1);
				}
				comma = false;
				result.fields[result.length] = parse_type(&start);
				result.length += 1;
			} else {
				if (start->variant != T_COMMA) {
					printf("Expected comma after type in struct type\n");
					exit(-1);
				}
				fputc(',', debug);
				comma = true;
				start += 1;
			}
		}
		fputc('}', debug);
	}
	*iter += 1;
	return result;
}

Binding parse_binding(TokenBranch **iter) {
	Binding result;
	// this line would make more sense in the language we're implementing
	// iter: **[TokenBranch];
	// ident: *TokenBranch := iter[0];
	TokenBranch *ident = *iter;
	if (ident->variant != T_ALPHANUM) { // *ident.variant != T_ALPHANUM
		printf("Expected identifier in parameter list\n");
		exit(-1);
	}
	result.ident = ident->data.substr;

	*iter += 1;
	if ((*iter)->variant != T_COLON) { // *iter[0].variant != T_COLON
		printf("Expected colon in parameter type ascription\n");
		exit(-1);
	}

	*iter += 1;
	fputsubstr(result.ident, debug);
	fputs(": ", debug);
	result.type = parse_type(iter);
	fputc('\n', debug);
	return result;
}

typedef enum {
	OPSTACK_INDEX_ADDITIVE,
} OperatorStackIndex;

typedef struct {
	enum {
		OPSTACK_ADDITIVE_EMPTY,
		OPSTACK_PLUS,
	} additive;
	unsigned derefs;
} OperatorStack;

void opstack_flush(
	OperatorStack *ops,
	OperatorStackIndex flush_until,
	Builder *out
) {
	switch (ops->additive) {
		case OPSTACK_ADDITIVE_EMPTY:
			break;
		case OPSTACK_PLUS:
			fputs(" +", debug);
			builder_push_u8(out, E_PLUS);
			break;
	}
	ops->additive = OPSTACK_ADDITIVE_EMPTY;
	for (int i = 0; i < ops->derefs; i++) {
		fputs(" *_", debug);
		builder_push_u8(out, E_DEREF);
	}
	ops->derefs = 0;
}

void parse_expr(TokenBranch **, Builder *, bool);

void parse_expr_once(TokenBranch **cb, Builder *out) {
	if ((*cb)->variant == T_ALPHANUM) {
		char c = (*cb)->data.substr.start[0];
		if ('0' <= c && c <= '9') {
			u64 data = 0;
			for (unsigned i = 0; i < (*cb)->data.substr.len; i++) {
				char c = (*cb)->data.substr.start[(*cb)->data.substr.len - i - 1];
				if ('0' <= c && c <= '9') {
					data *= 10;
					data += (u64)(c - '0');
				} else {
					printf("Found letter '%c' inside numeric constant\n", c);
					exit(-1);
				}
			}
			fprintf(debug, " %llu", data);
			builder_push_u8(out, E_INTEGER_LITERAL);
			builder_push_u64(out, data);
		} else {
			fputs(" \"", debug);
			fputsubstr((*cb)->data.substr, debug);
			fputs("\"", debug);
			builder_push_u8(out, E_IDENT);
			builder_push_substr(out, (*cb)->data.substr);
		}
	} else if ((*cb)->variant == T_OPEN_PAREN) {
		TokenTree *subtree = &(*cb)->data.subtree;
		TokenBranch *start = subtree->branches;
		TokenBranch *end = start + subtree->branch_num;
		parse_expr(&start, out, false);
		if (start != end) {
			printf("Syntax Error: Expected ')'.\n");
			exit(-1);
		}
	} else {
		printf("Syntax Error: Expected expression.\n");
		exit(-1);
	}
	++*cb;
}

void parse_expr(TokenBranch **cb, Builder *out, bool is_topmost) {
	OperatorStack ops = {};
	while ((*cb)->variant == T_ASTERISK) {
		ops.derefs += 1;
		++*cb;
	}
	// @Performance explicit stack instead of recursion
	parse_expr_once(cb, out);
	while (true) {
		TokenVariant t = (*cb)->variant;
		if (t == T_PLUS) {
			opstack_flush(&ops, OPSTACK_INDEX_ADDITIVE, out);
			++*cb;
			parse_expr_once(cb, out);
			ops.additive = OPSTACK_PLUS;
		} else {
			break;
		}
	}
	opstack_flush(&ops, 0, out);
	if (is_topmost) {
		builder_push_u8(out, E_END);
	}
}

Builder parse_body(TokenTree tt) {
	Builder result = {0,0,0};
	
	TokenBranch *cb = tt.branches;
	TokenBranch *end = cb + tt.branch_num;
	
	while (cb < end) {
		if (cb->variant == T_RETURN) {
			fputs("RET", debug);
			builder_push_u8(&result, S_RET);
			cb++;
			parse_expr(&cb, &result, true);
			fputs("\n", debug);
		} else if (cb->variant == T_LOCAL) {
			fputs("LOCAL ", debug);
			builder_push_u8(&result, S_LOCAL);
			cb++;
			Binding ident = parse_binding(&cb);
			builder_push_substr(&result, ident.ident);
		} else {
			builder_push_u8(&result, S_EVAL);
			builder_push_u8(&result, 0);
			char *length = builder_get_ptr(&result, result.length-1);
			builder_push_u8(&result, SL_DISCARD);
			char *loc = builder_get_ptr(&result, result.length-1);
			parse_expr(&cb, &result, true);
			if (cb->variant == T_DEFINITION) {
				fputs(" :=", debug);
				*length += 1;
				*loc = SL_DEF;
				cb++;
				builder_push_u8(&result, SL_DISCARD);
				parse_expr(&cb, &result, true);
			} else if (cb->variant == T_BIND) {
				fputs(" <-", debug);
				*length += 1;
				*loc = SL_WRITE;
				cb++;
				builder_push_u8(&result, SL_DISCARD);
				parse_expr(&cb, &result, true);
			}
			fputs("\n", debug);
		}
		if (cb->variant != T_SEMICOLON) {
			printf("Expected semicolon after statement\n");
			exit(-1);
		}
		cb++;
	}

	return result;
}

Items parse(TokenTree tt) {
	Items result;
	result.item_num = 0;
	result.items = (Proc*)malloc(sizeof (Proc) * (tt.branch_num / 3 + 1));

	TokenBranch *cb = tt.branches;
	TokenBranch *end = cb + tt.branch_num;

	while (cb < end) {
		Proc this;

		// f (...) -> ... {...}
		// 0 1     2      3+x
		if (cb + 3 < end && (cb + 2)->variant == T_ARROW) {
			this.name = cb->data.substr;
			if (cb->variant != T_ALPHANUM) {
				printf("Expected identifier after struct keyword");
				exit(-1);
			}
			this.name = cb->data.substr;

			cb++; //1
			if (cb->variant != T_OPEN_PAREN) {
				printf("Expected paren enclosed parameter list before proc definition");
				exit(-1);
			}
			{
				//this.params = cb->data.subtree;
				TokenTree subtree = cb->data.subtree;
				Binding *params = NULL;
				if (subtree.branch_num) {
					params = (Binding*)malloc(sizeof(Binding) * subtree.branch_num);
				}
				Binding *params_start = params;

				TokenBranch *pb = subtree.branches;
				TokenBranch *end = subtree.branches + subtree.branch_num;
				while (pb < end) {
					*params = parse_binding(&pb); // increases pb
					if (pb < end && pb->variant != T_COMMA) {
						printf("Expected comma in between parameters\n");
						exit(-1);
					}
					pb += 1;
					params += 1;
				}

				this.params = params;
				this.param_num = params - params_start;
			}

			cb+=2; // 3
			fputs("-> ", debug);
			this.ret_type = parse_type(&cb); // increases cb; 3+x
			fputs("\n\n", debug);

			if (cb->variant != T_OPEN_BRACE) {
				printf("Expected brace enclosed algorithm in proc definition");
				exit(-1);
			}
			this.body = parse_body(cb->data.subtree);

			cb++; // 3+x+1
		} else {
			printf("Currently only proc definitions are supported.");
			exit(-1);
		}
		result.items[result.item_num] = this;
		result.item_num++;
	}

	return result;
}

/*
char *render(Items in) {
	char *result = (char*)malloc(100);
	Proc *current = in.items;
	Proc *end = current + in.item_num;
	while (current < end) {
		// is this what strncpy really returns?
		result = strncpy(result, current->name.start, current->name.len);
		*result++ = '(';
		render_token_tree(result, current->params);
		*result++ = ')';
		result = strncpy(result, " { ", 3);
		result = render_token_tree(result, current->body);
		result = strncpy(result, " } ", 3);
	}
	return result;
}
*/

void destroy_ast(Items items) {
	fprintf(debug, "Leaked AST :)\n");
}

///////////////////////////
// compiler

typedef struct {
	enum {
		REF_LITERAL,
		REF_TEMPORARY,
		REF_VARIABLE,
	} variant;
	union {
		u64 val;
		substr ident;
	};
} Ref;

void print_substr(substr substr) {
	for (int i = 0; i < substr.len; i++) {
		putchar(substr.start[i]);
	}
}

void print_ref(Ref ref) {
	switch (ref.variant) {
		case REF_LITERAL:
			printf("%llu", ref.val);
			break;
		case REF_TEMPORARY:
			putchar('%');
			printf("%llu", ref.val);
			break;
		case REF_VARIABLE:
			putchar('%');
			print_substr(ref.ident);
			break;
	}
}

size_t compile_expr(
	Builder *data,
	unsigned *pi,
	u64 *temp_count,
	size_t stack_cap,
	Ref *stack
) {
	size_t stack_len = 0;
	bool overflow = false;
	while (true) {
		char c = builder_read_u8(data, *pi);
		++*pi;
		if (c == E_END) {
			break;
		}; if (c == E_IDENT) {
			if (stack_len == stack_cap) {
				overflow = true;
				break;
			}
			stack[stack_len].variant = REF_VARIABLE;
			stack[stack_len].ident = builder_read_substr(data, *pi);
			++stack_len;
			*pi += sizeof(substr);
		} else if (c == E_INTEGER_LITERAL) {
			if (stack_len == stack_cap) {
				overflow = true;
				break;
			}
			stack[stack_len].variant = REF_LITERAL;
			stack[stack_len].val = builder_read_u64(data, *pi);
			++stack_len;
			*pi += sizeof(u64);
		} else if (c == E_DEREF) {
			if (stack_len < 1) {
				printf("tried to compile unary operator with no expressions on stack\n");
				exit(-1);
			}
			Ref x = stack[stack_len-1];
			printf("    load i64, i64* ");
			print_ref(x);
			printf("\n");
			stack[stack_len-1].variant = REF_TEMPORARY;
			stack[stack_len-1].val = *temp_count;
			*temp_count += 1;
		} else if (c == E_PLUS) {
			if (stack_len < 2) {
				printf("tried to compile binary operator with less than 2 expressions on stack\n");
				exit(-1);
			}
			Ref l = stack[stack_len-2], r = stack[stack_len-1];
			printf("    add i64 ");
			print_ref(l);
			printf(", ");
			print_ref(r);
			printf("\n");
			stack_len -= 2;
			stack[stack_len].variant = REF_TEMPORARY;
			stack[stack_len].val = *temp_count;
			++*temp_count;
			++stack_len;
		} else {
			printf("Invalid expression discriminant: %d\n", c);
			exit(-1);
		}
	}
	if (overflow) {
		printf("Expression stack overflow (stack size was %lu)\n", stack_cap);
		exit(-1);
	}
	return stack_len;
}

substr program_read_ident(
	Builder *data,
	unsigned *pi
) {
	char cs = builder_read_u8(data, *pi);
	if (cs != E_IDENT) {
		printf("expected identifier? got %d\n", cs);
		exit(-1);
	}
	*pi += 1;
	substr result = builder_read_substr(data, *pi);
	*pi += sizeof(substr);
	char ce = builder_read_u8(data, *pi);
	if (ce != E_END) {
		printf("expected expression end?\n");
	}
	*pi += 1;
	return result;
}

void compile(Items items) {
	u64 temp_count = 1; // %0 is an anonymous jump label
	// @Robustness we could just store the required depth in the bytecode
	const size_t EXPR_STACK_CAPACITY = 16;
	Ref stack[EXPR_STACK_CAPACITY];
	for (int item = 0; item < items.item_num; item++) {
		Proc* proc = items.items + item;
		printf("define i64 @");	print_substr(proc->name); printf("() {\n");
		unsigned i = 0;
		while (i < proc->body.length) {
			switch(builder_read_u8(&proc->body, i++)) {
			case S_EVAL:
			{
				unsigned char len = builder_read_u8(&proc->body, i++);
				if (len != 1) {
					printf("eval statements are unimplemented.\n");
					exit(-1);
				}
				char sl = builder_read_u8(&proc->body, i++);
				if (sl == SL_DISCARD) {
					printf("eval output location was discard?\n");
					exit(-1);
				}
				Ref out;
				if (sl == SL_DEF) {
					out.variant = REF_VARIABLE;
					out.ident = program_read_ident(&proc->body, &i);
				} else if (sl == SL_WRITE) {
					size_t slen = compile_expr(&proc->body, &i,
							&temp_count, EXPR_STACK_CAPACITY, stack);
					if (slen != 1) {
						printf("lhs of write expression had %d values\n", slen);
						exit(-1);
					}
					out = stack[0];
				} else {
					printf("unknown output location code: %d\n", sl);
					exit(-1);
				}
				char sl_last = builder_read_u8(&proc->body, i++);
				if (sl_last != SL_DISCARD) {
					printf("eval/rhs was not discard?\n");
					exit(-1);
				}
				size_t slen = compile_expr(&proc->body, &i,
						&temp_count, EXPR_STACK_CAPACITY, stack);
				if (slen != 1) {
					printf("rhs of statement had %d values\n", slen);
					exit(-1);
				}
				Ref val = stack[0];
				if (sl == SL_DEF) {
					printf("    ");
					print_ref(out);
					printf(" = ");
					print_ref(val);
					printf("\n");
				} else {
					printf("    store i64 ");
					print_ref(val);
					printf(", i64* ");
					print_ref(out);
					printf("\n");
				}
				break;
			}
			case S_LOCAL:
			{
				substr ident = builder_read_substr(&proc->body, i);
				i += sizeof(substr);
				printf("    %%");
				fputsubstr(ident, stdout);
				printf(" = alloca i64\n");
				break;
			}
			case S_RET:
			{
				size_t len = compile_expr(&proc->body, &i,
						&temp_count, EXPR_STACK_CAPACITY, stack);
				if (len != 1) {
					printf("Expected one expression in return statement, got: %llu\n", len);
					exit(-1);
				}
				printf("    ret i64 ");
				print_ref(stack[0]);
				printf("\n");
				break;
			}
			default:
				printf("Invalid statement discriminant: %d\n", *builder_get_ptr(&proc->body, i));
				exit(-1);
			}
		}
		printf("}\n");
	}
}

///////////////////////////
// test(s)

void test() {
	// @Bug put spaces in here and check tokenizer still works
	char *input = "\
main(argc: b64, argv: *[{ b64, *[b8] }]) -> b64 {\
	local x: b64;\
	x <- 1;\
	return *x + 2;\
}";
	fputs("Source:\n", debug);
	fputs(input, debug);
	fputc('\n', debug);
	unsigned long len = strlen(input);

	fprintf(debug, "Tokenizing:\n");
	TokenTree ts = tokenize_flat(input, len);

	for (int i = 0; i < ts.branch_num; i++) {
		fprintf(debug, " %d", ts.branches[i].variant);
	}
	fprintf(debug, "\n");

	fprintf(debug, "Grouping...\n");
	TokenTree tt = group_tokens(ts);
	destroy_tt(ts);

	{
// @Robustness measure actual depth of token tree? store depth?
#define GROUP_DEBUG_STACK_LEN 32
		TokenTree trees[GROUP_DEBUG_STACK_LEN];
		int indeces[GROUP_DEBUG_STACK_LEN];
		int s = 0;
		trees[0] = tt;
		indeces[0] = 0;
		while (true) {
			if (indeces[s] >= trees[s].branch_num) {
				s -= 1;
				if (s < 0) {
					break;
				}
				TokenVariant v = trees[s].branches[indeces[s]].variant;
				fputc('\n', debug);
				for (int i = 0; i < s; i++) {
					fputs("    ", debug);
				}
				fputc(' ', debug);
				fprintf(debug, "%s", utokens[v + T_TOTAL_OPENS].str);
				indeces[s] += 1;
			} else {
				TokenBranch *branch = &trees[s].branches[indeces[s]];
				TokenVariant v = branch->variant;
				fputc(' ', debug);
				if (v == T_ALPHANUM) {
					fputc('"', debug);
					fputsubstr(branch->data.substr, debug);
					fputc('"', debug);
				} else {
					fprintf(debug, "%s", utokens[v].str);
				}
				if (T_FIRST_OPEN <= v && v <= T_LAST_OPEN) {
					s += 1;
					if (s >= GROUP_DEBUG_STACK_LEN) {
						printf("Token tree too deep for print routine!\n");
						exit(-1);
					}
					trees[s] = branch->data.subtree;
					indeces[s] = 0;
					fputc('\n', debug);
					for (int i = 0; i < s; i++) {
						fputs("    ", debug);
					}
				} else {
					indeces[s]++;
				}
			}
		}
	}
	fputc('\n', debug);

	fprintf(debug, "Parsing...\n");
	Items items = parse(tt);
	fputs("\n", debug);
	destroy_tt(tt);
	for (int i = 0; i < items.item_num; i++) {
		builder_log(&items.items[i].body);
	}

	fprintf(debug, "Compiling...\n");
	compile(items);
	destroy_ast(items);
}

int main() {
	debug = fopen("output/debug.txt", "w");  // errors, who cares
	test();
	fclose(debug);
}
