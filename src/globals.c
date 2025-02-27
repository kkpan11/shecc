/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>
#include <stdlib.h>

/* Global objects */

block_list_t BLOCKS;

macro_t *MACROS;
int macros_idx = 0;

/* FUNCS_MAP is used to integrate function storing and boost lookup
 * performance, currently it uses FNV-1a hash function to hash function
 * name. The bucket size defaults to MAX_FUNCS. Ideally, it should be a small
 * number, but due to lack of rehashing implementation, to prevent collision,
 * we have to initially create large amount of buckets.
 */
hashmap_t *FUNCS_MAP;

type_t *TYPES;
int types_idx = 0;

ph1_ir_t *GLOBAL_IR;
int global_ir_idx = 0;

ph1_ir_t *PH1_IR;
int ph1_ir_idx = 0;

ph2_ir_t *PH2_IR;
int ph2_ir_idx = 0;

label_lut_t *LABEL_LUT;
int label_lut_idx = 0;

func_list_t FUNC_LIST;
func_t GLOBAL_FUNC;
basic_block_t *MAIN_BB;
int elf_offset = 0;

regfile_t REGS[REG_CNT];

alias_t *ALIASES;
int aliases_idx = 0;

constant_t *CONSTANTS;
int constants_idx = 0;

char *SOURCE;
int source_idx = 0;

/* ELF sections */

char *elf_code;
int elf_code_idx = 0;
char *elf_data;
int elf_data_idx = 0;
char *elf_header;
int elf_header_idx = 0;
int elf_header_len = 0x54; /* ELF fixed: 0x34 + 1 * 0x20 */
int elf_code_start;
int elf_data_start;
char *elf_symtab;
char *elf_strtab;
char *elf_section;

/**
 * hashmap_hash_index() - hashses a string with FNV-1a hash function
 * and converts into usable hashmap index. The range of returned
 * hashmap index is ranged from "(0 ~ 2,147,483,647) mod size" due to
 * lack of unsigned integer implementation.
 * @size: The size of map. Must not be negative or 0.
 * @key: The key string. May be NULL.
 *
 * Return: The usable hashmap index.
 */
int hashmap_hash_index(int size, char *key)
{
    int hash = 0x811c9dc5, mask;

    for (; *key; key++) {
        hash ^= *key;
        hash *= 0x01000193;
    }

    mask = hash >> 31;
    return ((hash ^ mask) - mask) & (size - 1);
}

int round_up_pow2(int v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

/**
 * hashmap_create() - creates a hashmap on heap. Notice that
 * provided size will always be rounded up to nearest power of 2.
 * @size: The initial bucket size of hashmap. Must not be 0 or
 * negative.
 *
 * Return: The pointer of created hashmap.
 */
hashmap_t *hashmap_create(int size)
{
    hashmap_t *map = malloc(sizeof(hashmap_t));

    if (!map) {
        printf("Failed to allocate hashmap_t with size %d\n", size);
        return NULL;
    }

    map->size = round_up_pow2(size);
    map->buckets = malloc(map->size * sizeof(hashmap_node_t *));

    if (!map->buckets) {
        printf("Failed to allocate buckets in hashmap_t\n");
        free(map);
        return NULL;
    }

    for (int i = 0; i < map->size; i++)
        map->buckets[i] = 0;

    return map;
}

/**
 * hashmap_node_new() - creates a hashmap node on heap.
 * @key: The key of node. Must not be NULL.
 * @val: The value of node. Could be NULL.
 *
 * Return: The pointer of created node.
 */
hashmap_node_t *hashmap_node_new(char *key, void *val)
{
    if (!key)
        return NULL;

    int len = strlen(key);
    hashmap_node_t *node = malloc(sizeof(hashmap_node_t));


    if (!node) {
        printf("Failed to allocate hashmap_node_t\n");
        return NULL;
    }

    node->key = calloc(len + 1, sizeof(char));

    if (!node->key) {
        printf("Failed to allocate hashmap_node_t key with size %d\n");
        free(node);
        return NULL;
    }

    strcpy(node->key, key);
    node->val = val;
    node->next = NULL;
    return node;
}

/**
 * hashmap_put() - puts a key-value pair into given hashmap.
 * If key already contains a value, then replace it with new
 * value, the old value will be freed.
 * @map: The hashmap to be put into. Must not be NULL.
 * @key: The key string. May be NULL.
 * @val: The value pointer. May be NULL. This value's lifetime
 * is held by hashmap.
 */
void hashmap_put(hashmap_t *map, char *key, void *val)
{
    if (!map)
        return;

    int index = hashmap_hash_index(map->size, key);
    hashmap_node_t *cur = map->buckets[index];

    if (!cur) {
        map->buckets[index] = hashmap_node_new(key, val);
    } else {
        while (cur->next)
            cur = cur->next;
        cur->next = hashmap_node_new(key, val);
    }

    /* TODO: Rehash if size exceeds size * load factor */
}

/**
 * hashmap_get() - gets value from hashmap from given key.
 * @map: The hashmap to be looked up. Must no be NULL.
 * @key: The key string. May be NULL.
 *
 * Return: The look up result, if the key-value pair entry
 * exists, then returns its value's address, NULL otherwise.
 */
void *hashmap_get(hashmap_t *map, char *key)
{
    if (!map)
        return NULL;

    int index = hashmap_hash_index(map->size, key);

    for (hashmap_node_t *cur = map->buckets[index]; cur; cur = cur->next)
        if (!strcmp(cur->key, key))
            return cur->val;

    return NULL;
}

/**
 * hashmap_contains() - checks if the key-value pair entry exists
 * from given key.
 * @map: The hashmap to be looked up. Must no be NULL.
 * @key: The key string. May be NULL.
 *
 * Return: The look up result, if the key-value pair entry
 * exists, then returns true, false otherwise.
 */
bool hashmap_contains(hashmap_t *map, char *key)
{
    return hashmap_get(map, key);
}

/**
 * hashmap_free() - frees the hashmap, this also frees key-value pair
 * entry's value.
 * @map: The hashmap to be looked up. Must no be NULL.
 */
void hashmap_free(hashmap_t *map)
{
    if (!map)
        return;

    for (int i = 0; i < map->size; i++) {
        for (hashmap_node_t *cur = map->buckets[i], *next; cur; cur = next) {
            next = cur->next;
            free(cur->key);
            free(cur->val);
            /* FIXME: Remove this if-clause will cause double free error */
            if (cur != map->buckets[0])
                free(cur);
            cur = next;
        }
    }

    free(map->buckets);
    free(map);
}

/* options */

int dump_ir = 0;
int hard_mul_div = 0;

/**
 * find_type() - Find the type by the given name.
 * @type_name: The name to be searched.
 * @flag:
 *      0 - Search in all type names.
 *      1 - Search in all names, excluding the tags of structure.
 *      2 - Only search in tags.
 *
 * Return: The pointer to the type, or NULL if not found.
 */
type_t *find_type(char *type_name, int flag)
{
    for (int i = 0; i < types_idx; i++) {
        if (TYPES[i].base_type == TYPE_struct) {
            if (flag == 1)
                continue;
            if (!strcmp(TYPES[i].type_name, type_name))
                return &TYPES[i];
        } else {
            if (flag == 2)
                continue;
            if (!strcmp(TYPES[i].type_name, type_name)) {
                /* If it is a forwardly declared alias of a structure, return
                 * the base structure type.
                 */
                if (TYPES[i].base_type == TYPE_typedef && TYPES[i].size == 0)
                    return TYPES[i].base_struct;
                return &TYPES[i];
            }
        }
    }
    return NULL;
}

ph1_ir_t *add_global_ir(opcode_t op)
{
    ph1_ir_t *ir = &GLOBAL_IR[global_ir_idx++];
    ir->op = op;
    return ir;
}

ph1_ir_t *add_ph1_ir(opcode_t op)
{
    ph1_ir_t *ph1_ir = &PH1_IR[ph1_ir_idx++];
    ph1_ir->op = op;
    return ph1_ir;
}

ph2_ir_t *add_ph2_ir(opcode_t op)
{
    ph2_ir_t *ph2_ir = &PH2_IR[ph2_ir_idx++];
    ph2_ir->op = op;
    return ph2_ir;
}

void set_var_liveout(var_t *var, int end)
{
    if (var->liveness >= end)
        return;
    var->liveness = end;
}

void add_label(char *name, int offset)
{
    label_lut_t *lut = &LABEL_LUT[label_lut_idx++];
    strcpy(lut->name, name);
    lut->offset = offset;
}

int find_label_offset(char name[])
{
    for (int i = 0; i < label_lut_idx; i++) {
        if (!strcmp(LABEL_LUT[i].name, name))
            return LABEL_LUT[i].offset;
    }
    return -1;
}

block_t *add_block(block_t *parent, func_t *func, macro_t *macro)
{
    block_t *blk = malloc(sizeof(block_t));

    if (!BLOCKS.head) {
        BLOCKS.head = blk;
        BLOCKS.tail = BLOCKS.head;
    } else {
        BLOCKS.tail->next = blk;
        BLOCKS.tail = blk;
    }

    blk->parent = parent;
    blk->func = func;
    blk->macro = macro;
    blk->next_local = 0;
    return blk;
}

void add_alias(char *alias, char *value)
{
    alias_t *al = &ALIASES[aliases_idx++];
    strcpy(al->alias, alias);
    strcpy(al->value, value);
    al->disabled = false;
}

char *find_alias(char alias[])
{
    for (int i = 0; i < aliases_idx; i++) {
        if (!ALIASES[i].disabled && !strcmp(alias, ALIASES[i].alias))
            return ALIASES[i].value;
    }
    return NULL;
}

bool remove_alias(char *alias)
{
    for (int i = 0; i < aliases_idx; i++) {
        if (!ALIASES[i].disabled && !strcmp(alias, ALIASES[i].alias)) {
            ALIASES[i].disabled = true;
            return true;
        }
    }
    return false;
}

macro_t *add_macro(char *name)
{
    macro_t *ma = &MACROS[macros_idx++];
    strcpy(ma->name, name);
    ma->disabled = false;
    return ma;
}

macro_t *find_macro(char *name)
{
    for (int i = 0; i < macros_idx; i++) {
        if (!MACROS[i].disabled && !strcmp(name, MACROS[i].name))
            return &MACROS[i];
    }
    return NULL;
}

bool remove_macro(char *name)
{
    for (int i = 0; i < macros_idx; i++) {
        if (!MACROS[i].disabled && !strcmp(name, MACROS[i].name)) {
            MACROS[i].disabled = true;
            return true;
        }
    }
    return false;
}

void error(char *msg);
int find_macro_param_src_idx(char *name, block_t *parent)
{
    macro_t *macro = parent->macro;

    if (!parent)
        error("The macro expansion is not supported in the global scope");
    if (!parent->macro)
        return 0;

    for (int i = 0; i < macro->num_param_defs; i++) {
        if (!strcmp(macro->param_defs[i].var_name, name))
            return macro->params[i];
    }
    return 0;
}

func_t *add_func(char *name)
{
    func_t *fn;
    if (hashmap_contains(FUNCS_MAP, name)) {
        fn = hashmap_get(FUNCS_MAP, name);
    } else {
        fn = malloc(sizeof(func_t));

        if (!fn) {
            printf("Failed to allocate func_t\n");
            return NULL;
        }

        hashmap_put(FUNCS_MAP, name, fn);
        strcpy(fn->return_def.var_name, name);
    }

    fn->stack_size = 4; /* starting point of stack */
    return fn;
}

type_t *add_type()
{
    return &TYPES[types_idx++];
}

type_t *add_named_type(char *name)
{
    type_t *type = add_type();
    strcpy(type->type_name, name);
    return type;
}

void add_constant(char alias[], int value)
{
    constant_t *constant = &CONSTANTS[constants_idx++];
    strcpy(constant->alias, alias);
    constant->value = value;
}

constant_t *find_constant(char alias[])
{
    for (int i = 0; i < constants_idx; i++) {
        if (!strcmp(CONSTANTS[i].alias, alias))
            return &CONSTANTS[i];
    }
    return NULL;
}

func_t *find_func(char func_name[])
{
    return hashmap_get(FUNCS_MAP, func_name);
}

var_t *find_member(char token[], type_t *type)
{
    /* If it is a forwardly declared alias of a structure, switch to the base
     * structure type.
     */
    if (type->size == 0)
        type = type->base_struct;

    for (int i = 0; i < type->num_fields; i++) {
        if (!strcmp(type->fields[i].var_name, token))
            return &type->fields[i];
    }
    return NULL;
}

var_t *find_local_var(char *token, block_t *block)
{
    func_t *fn = block->func;

    for (; block; block = block->parent) {
        for (int i = 0; i < block->next_local; i++) {
            if (!strcmp(block->locals[i].var_name, token))
                return &block->locals[i];
        }
    }

    if (fn) {
        for (int i = 0; i < fn->num_params; i++) {
            if (!strcmp(fn->param_defs[i].var_name, token))
                return &fn->param_defs[i];
        }
    }
    return NULL;
}

var_t *find_global_var(char *token)
{
    block_t *block = BLOCKS.head;

    for (int i = 0; i < block->next_local; i++) {
        if (!strcmp(block->locals[i].var_name, token))
            return &block->locals[i];
    }
    return NULL;
}

var_t *find_var(char *token, block_t *parent)
{
    var_t *var = find_local_var(token, parent);
    if (!var)
        var = find_global_var(token);
    return var;
}

int size_var(var_t *var)
{
    int size;
    if (var->is_ptr > 0 || var->is_func) {
        size = 4;
    } else {
        type_t *type = find_type(var->type_name, 0);
        if (!type)
            error("Incomplete type");
        if (type->size == 0)
            size = type->base_struct->size;
        else
            size = type->size;
    }
    if (var->array_size > 0)
        size = size * var->array_size;
    return size;
}

/* TODO: Integrate with 'func_t' */
fn_t *add_fn()
{
    fn_t *n = calloc(1, sizeof(fn_t));

    if (!FUNC_LIST.head) {
        FUNC_LIST.head = n;
        FUNC_LIST.tail = n;
        return n;
    }
    FUNC_LIST.tail->next = n;
    FUNC_LIST.tail = n;
    return n;
}

/* Create a basic block and set the scope of variables to 'parent' block */
basic_block_t *bb_create(block_t *parent)
{
    basic_block_t *bb = calloc(1, sizeof(basic_block_t));

    for (int i = 0; i < MAX_BB_PRED; i++) {
        bb->prev[i].bb = NULL;
        bb->prev[i].type = NEXT;
    }
    bb->scope = parent;
    bb->belong_to = parent->func->fn;
    return bb;
}

/* The pred-succ pair must have only one connection */
void bb_connect(basic_block_t *pred,
                basic_block_t *succ,
                bb_connection_type_t type)
{
    if (!pred)
        abort();
    if (!succ)
        abort();

    int i = 0;
    while (succ->prev[i].bb)
        i++;

    if (i > MAX_BB_PRED - 1) {
        printf("Error: too many predecessors\n");
        abort();
    }

    succ->prev[i].bb = pred;
    succ->prev[i].type = type;

    switch (type) {
    case NEXT:
        pred->next = succ;
        break;
    case THEN:
        pred->then_ = succ;
        break;
    case ELSE:
        pred->else_ = succ;
        break;
    default:
        abort();
    }
}

/* The pred-succ pair must have only one connection */
void bb_disconnect(basic_block_t *pred, basic_block_t *succ)
{
    for (int i = 0; i < MAX_BB_PRED; i++) {
        if (succ->prev[i].bb == pred) {
            switch (succ->prev[i].type) {
            case NEXT:
                pred->next = NULL;
                break;
            case THEN:
                pred->then_ = NULL;
                break;
            case ELSE:
                pred->else_ = NULL;
                break;
            default:
                abort();
            }

            succ->prev[i].bb = NULL;
            break;
        }
    }
}

/* The symbol is an argument of function or the variable in declaration */
void add_symbol(basic_block_t *bb, var_t *var)
{
    if (!bb)
        return;
    symbol_t *sym;
    for (sym = bb->symbol_list.head; sym; sym = sym->next) {
        if (sym->var == var)
            return;
    }

    sym = calloc(1, sizeof(symbol_t));
    sym->var = var;

    if (!bb->symbol_list.head) {
        sym->index = 0;
        bb->symbol_list.head = sym;
        bb->symbol_list.tail = sym;
    } else {
        sym->index = bb->symbol_list.tail->index + 1;
        bb->symbol_list.tail->next = sym;
        bb->symbol_list.tail = sym;
    }
}

void add_insn(block_t *block,
              basic_block_t *bb,
              opcode_t op,
              var_t *rd,
              var_t *rs1,
              var_t *rs2,
              int sz,
              char *str)
{
    if (!bb)
        return;

    bb->scope = block;

    insn_t *n = calloc(1, sizeof(insn_t));
    n->opcode = op;
    n->rd = rd;
    n->rs1 = rs1;
    n->rs2 = rs2;
    n->sz = sz;
    n->belong_to = bb;

    if (str)
        strcpy(n->str, str);

    if (!bb->insn_list.head)
        bb->insn_list.head = n;
    else
        bb->insn_list.tail->next = n;

    n->prev = bb->insn_list.tail;
    bb->insn_list.tail = n;
}

/* This routine is required because the global variable initializations are
 * not supported now.
 */
void global_init()
{
    elf_code_start = ELF_START + elf_header_len;

    BLOCKS.head = NULL;
    BLOCKS.tail = NULL;
    MACROS = malloc(MAX_ALIASES * sizeof(macro_t));
    FUNCS_MAP = hashmap_create(MAX_FUNCS);
    TYPES = malloc(MAX_TYPES * sizeof(type_t));
    GLOBAL_IR = malloc(MAX_GLOBAL_IR * sizeof(ph1_ir_t));
    PH1_IR = malloc(MAX_IR_INSTR * sizeof(ph1_ir_t));
    PH2_IR = malloc(MAX_IR_INSTR * sizeof(ph2_ir_t));
    LABEL_LUT = malloc(MAX_LABEL * sizeof(label_lut_t));
    SOURCE = malloc(MAX_SOURCE);
    ALIASES = malloc(MAX_ALIASES * sizeof(alias_t));
    CONSTANTS = malloc(MAX_CONSTANTS * sizeof(constant_t));

    elf_code = malloc(MAX_CODE);
    elf_data = malloc(MAX_DATA);
    elf_header = malloc(MAX_HEADER);
    elf_symtab = malloc(MAX_SYMTAB);
    elf_strtab = malloc(MAX_STRTAB);
    elf_section = malloc(MAX_SECTION);

    /* set starting point of global stack manually */
    func_t *global_func = add_func("");
    global_func->stack_size = 4;
}

void global_release()
{
    while (BLOCKS.head) {
        block_t *next = BLOCKS.head->next;
        free(BLOCKS.head);
        BLOCKS.head = next;
    }
    free(MACROS);
    hashmap_free(FUNCS_MAP);
    free(TYPES);
    free(GLOBAL_IR);
    free(PH1_IR);
    free(PH2_IR);
    free(LABEL_LUT);
    free(SOURCE);
    free(ALIASES);
    free(CONSTANTS);

    free(elf_code);
    free(elf_data);
    free(elf_header);
    free(elf_symtab);
    free(elf_strtab);
    free(elf_section);
}

void error(char *msg)
{
    /* Construct error source diagnostics, enabling precise identification of
     * syntax and logic issues within the code.
     */
    int offset, start_idx, i = 0;
    char diagnostic[512 /* MAX_LINE_LEN * 2 */];

    for (offset = source_idx; offset >= 0 && SOURCE[offset] != '\n'; offset--)
        ;

    start_idx = offset + 1;

    for (offset = 0; offset < MAX_SOURCE && SOURCE[start_idx + offset] != '\n';
         offset++) {
        diagnostic[i++] = SOURCE[start_idx + offset];
    }
    diagnostic[i++] = '\n';

    for (offset = start_idx; offset < source_idx; offset++) {
        diagnostic[i++] = ' ';
    }

    strcpy(diagnostic + i, "^ Error occurs here");

    /* TODO: figure out the corresponding C source file path and report line
     * number.
     */
    printf("Error %s at source location %d\n%s\n", msg, source_idx, diagnostic);
    abort();
}

void print_indent(int indent)
{
    for (int i = 0; i < indent; i++)
        printf("\t");
}

void dump_ph1_ir()
{
    int indent = 0;
    ph1_ir_t *ph1_ir;
    func_t *fn;
    char rd[MAX_VAR_LEN], op1[MAX_VAR_LEN], op2[MAX_VAR_LEN];

    for (int i = 0; i < ph1_ir_idx; i++) {
        ph1_ir = &PH1_IR[i];

        if (ph1_ir->dest)
            strcpy(rd, ph1_ir->dest->var_name);
        if (ph1_ir->src0)
            strcpy(op1, ph1_ir->src0->var_name);
        if (ph1_ir->src1)
            strcpy(op2, ph1_ir->src1->var_name);

        switch (ph1_ir->op) {
        case OP_define:
            fn = find_func(ph1_ir->func_name);
            printf("def %s", fn->return_def.type_name);

            for (int j = 0; j < fn->return_def.is_ptr; j++)
                printf("*");
            printf(" @%s(", ph1_ir->func_name);

            for (int j = 0; j < fn->num_params; j++) {
                if (j != 0)
                    printf(", ");
                printf("%s", fn->param_defs[j].type_name);

                for (int k = 0; k < fn->param_defs[j].is_ptr; k++)
                    printf("*");
                printf(" %%%s", fn->param_defs[j].var_name);
            }
            printf(")");
            break;
        case OP_block_start:
            print_indent(indent);
            printf("{");
            indent++;
            break;
        case OP_block_end:
            indent--;
            print_indent(indent);
            printf("}");
            break;
        case OP_allocat:
            print_indent(indent);
            printf("allocat %s", ph1_ir->src0->type_name);
            for (int j = 0; j < ph1_ir->src0->is_ptr; j++)
                printf("*");
            printf(" %%%s", op1);

            if (ph1_ir->src0->array_size > 0)
                printf("[%d]", ph1_ir->src0->array_size);
            break;
        case OP_load_constant:
            print_indent(indent);
            printf("const %%%s, $%d", rd, ph1_ir->dest->init_val);
            break;
        case OP_load_data_address:
            print_indent(indent);
            /* offset from .data section */
            printf("%%%s = .data (%d)", rd, ph1_ir->dest->init_val);
            break;
        case OP_address_of:
            print_indent(indent);
            printf("%%%s = &(%%%s)", rd, op1);
            break;
        case OP_assign:
            print_indent(indent);
            printf("%%%s = %%%s", rd, op1);
            break;
        case OP_label:
            print_indent(0);
            printf("%s", op1);
            break;
        case OP_branch:
            print_indent(indent);
            printf("br %%%s, %s, %s", rd, op1, op2);
            break;
        case OP_jump:
            print_indent(indent);
            printf("j %s", rd);
            break;
        case OP_push:
            print_indent(indent);
            printf("push %%%s", op1);
            break;
        case OP_call:
            print_indent(indent);
            printf("call @%s, %d", ph1_ir->func_name, ph1_ir->param_num);
            break;
        case OP_func_ret:
            print_indent(indent);
            printf("retval %%%s", rd);
            break;
        case OP_return:
            print_indent(indent);
            if (ph1_ir->src0)
                printf("ret %%%s", op1);
            else
                printf("ret");
            break;
        case OP_read:
            print_indent(indent);
            printf("%%%s = (%%%s), %d", rd, op1, ph1_ir->size);
            break;
        case OP_write:
            print_indent(indent);
            if (ph1_ir->src0->is_func)
                printf("(%%%s) = @%s", rd, op1);
            else
                printf("(%%%s) = %%%s, %d", rd, op1, ph1_ir->size);
            break;
        case OP_indirect:
            print_indent(indent);
            printf("indirect call @(%%%s)", op1);
            break;
        case OP_negate:
            print_indent(indent);
            printf("neg %%%s, %%%s", rd, op1);
            break;
        case OP_add:
            print_indent(indent);
            printf("%%%s = add %%%s, %%%s", rd, op1, op2);
            break;
        case OP_sub:
            print_indent(indent);
            printf("%%%s = sub %%%s, %%%s", rd, op1, op2);
            break;
        case OP_mul:
            print_indent(indent);
            printf("%%%s = mul %%%s, %%%s", rd, op1, op2);
            break;
        case OP_div:
            print_indent(indent);
            printf("%%%s = div %%%s, %%%s", rd, op1, op2);
            break;
        case OP_mod:
            print_indent(indent);
            printf("%%%s = mod %%%s, %%%s", rd, op1, op2);
            break;
        case OP_eq:
            print_indent(indent);
            printf("%%%s = eq %%%s, %%%s", rd, op1, op2);
            break;
        case OP_neq:
            print_indent(indent);
            printf("%%%s = neq %%%s, %%%s", rd, op1, op2);
            break;
        case OP_gt:
            print_indent(indent);
            printf("%%%s = gt %%%s, %%%s", rd, op1, op2);
            break;
        case OP_lt:
            print_indent(indent);
            printf("%%%s = lt %%%s, %%%s", rd, op1, op2);
            break;
        case OP_geq:
            print_indent(indent);
            printf("%%%s = geq %%%s, %%%s", rd, op1, op2);
            break;
        case OP_leq:
            print_indent(indent);
            printf("%%%s = leq %%%s, %%%s", rd, op1, op2);
            break;
        case OP_bit_and:
            print_indent(indent);
            printf("%%%s = and %%%s, %%%s", rd, op1, op2);
            break;
        case OP_bit_or:
            print_indent(indent);
            printf("%%%s = or %%%s, %%%s", rd, op1, op2);
            break;
        case OP_bit_not:
            print_indent(indent);
            printf("%%%s = not %%%s", rd, op1);
            break;
        case OP_bit_xor:
            print_indent(indent);
            printf("%%%s = xor %%%s, %%%s", rd, op1, op2);
            break;
        case OP_log_and:
            print_indent(indent);
            printf("%%%s = and %%%s, %%%s", rd, op1, op2);
            break;
        case OP_log_or:
            print_indent(indent);
            printf("%%%s = or %%%s, %%%s", rd, op1, op2);
            break;
        case OP_log_not:
            print_indent(indent);
            printf("%%%s = not %%%s", rd, op1);
            break;
        case OP_rshift:
            print_indent(indent);
            printf("%%%s = rshift %%%s, %%%s", rd, op1, op2);
            break;
        case OP_lshift:
            print_indent(indent);
            printf("%%%s = lshift %%%s, %%%s", rd, op1, op2);
            break;
        default:
            break;
        }
        printf("\n");
    }
    printf("===\n");
}
