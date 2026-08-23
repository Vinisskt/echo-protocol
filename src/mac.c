#include "../include/mac.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

static uint32_t default_get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

void mac_init(MacContext *ctx, uint8_t node_id, uint8_t num_nodes,
              uint8_t slot_count, uint32_t slot_duration_us,
              uint32_t (*get_time_us)(void)) {
    memset(ctx, 0, sizeof(MacContext));
    
    ctx->node_id = node_id;
    ctx->num_nodes = num_nodes > MAC_MAX_NODES ? MAC_MAX_NODES : num_nodes;
    ctx->slot_count = slot_count > 0 ? slot_count : 1;
    ctx->slot_duration_us = slot_duration_us > 0 ? slot_duration_us : 10000;
    ctx->fixed_dt_us = ctx->slot_duration_us;
    
    ctx->get_time_us = get_time_us ? get_time_us : default_get_time_us;
    ctx->frame_start_time = ctx->get_time_us();
    
    /* Q-learning defaults */
    ctx->alpha = 0.1f;
    ctx->gamma = 0.9f;
    ctx->epsilon = 0.3f;
    ctx->mode = MAC_MODE_HYBRID;
    
    /* Initialize Q-table with small random values */
    for (int i = 0; i < MAC_Q_TABLE_SIZE; i++) {
        for (int a = 0; a < MAC_MAX_ACTIONS; a++) {
            ctx->q_table[i].q_values[a] = ((float)rand() / RAND_MAX - 0.5f) * 0.01f;
        }
    }
    
    /* Create default behavior tree */
    ctx->bt_root = mac_create_default_bt(ctx);
}

void mac_set_mode(MacContext *ctx, MacMode mode) {
    ctx->mode = mode;
}

/* Fixed timestep update (game loop pattern) */
void mac_update(MacContext *ctx, uint32_t dt_us) {
    ctx->accumulator_us += dt_us;
    
    while (ctx->accumulator_us >= ctx->fixed_dt_us) {
        ctx->accumulator_us -= ctx->fixed_dt_us;
        ctx->current_slot = (ctx->current_slot + 1) % ctx->slot_count;
        
        if (ctx->current_slot == 0) {
            ctx->frame_start_time += ctx->slot_duration_us * ctx->slot_count;
        }
    }
}

void mac_on_channel_busy(MacContext *ctx, bool busy) {
    (void)ctx;
    (void)busy;
}

void mac_on_tx_result(MacContext *ctx, bool success, bool collision, bool nak) {
    float reward = 0;
    MacState next_state = MAC_STATE_IDLE;
    
    if (success) {
        reward = 1.0f;
        next_state = MAC_STATE_ACKED;
        ctx->tx_success++;
    } else if (collision) {
        reward = -1.0f;
        next_state = MAC_STATE_COLLISION;
        ctx->tx_collision++;
    } else if (nak) {
        reward = -0.5f;
        next_state = MAC_STATE_NAK;
    } else {
        reward = -2.0f;
        next_state = MAC_STATE_BACKOFF;
        ctx->tx_timeout++;
    }
    
    ctx->tx_attempts++;
    
    /* Q-learning update */
    if (ctx->mode == MAC_MODE_Q_LEARNING || ctx->mode == MAC_MODE_HYBRID) {
        mac_q_learn(ctx, ctx->last_state, ctx->last_action, reward, next_state);
    }
    
    ctx->last_state = next_state;
}

void mac_on_queue_change(MacContext *ctx, bool has_data) {
    if (has_data && ctx->last_state == MAC_STATE_IDLE) {
        ctx->last_state = MAC_STATE_QUEUED;
    } else if (!has_data) {
        ctx->last_state = MAC_STATE_IDLE;
    }
}

MacAction mac_get_action(MacContext *ctx) {
    MacState state = ctx->last_state;
    
    /* Determine state based on current conditions */
    if (state == MAC_STATE_IDLE) {
        return MAC_ACTION_WAIT;
    }
    
    MacAction action = MAC_ACTION_WAIT;
    
    switch (ctx->mode) {
        case MAC_MODE_FIXED_TDMA:
            /* Fixed TDMA: transmit in assigned slot */
            if (ctx->current_slot == ctx->node_id % ctx->slot_count) {
                action = MAC_ACTION_TX_NOW;
            } else {
                action = MAC_ACTION_WAIT;
            }
            break;
            
        case MAC_MODE_Q_LEARNING:
            action = mac_q_select_action(ctx, state);
            break;
            
        case MAC_MODE_BEHAVIOR_TREE:
            action = bt_execute(ctx->bt_root, ctx);
            ctx->bt_decisions++;
            break;
            
        case MAC_MODE_HYBRID:
            /* Try Q-learning first, fallback to BT */
            action = mac_q_select_action(ctx, state);
            if (action == MAC_ACTION_WAIT && ctx->bt_root) {
                MacAction bt_action = bt_execute(ctx->bt_root, ctx);
                if (bt_action != MAC_ACTION_WAIT) action = bt_action;
                ctx->bt_decisions++;
            }
            break;
    }
    
    ctx->last_action = action;
    ctx->last_state_idx = mac_state_to_idx(state);
    
    return action;
}

uint32_t mac_state_to_idx(MacState state) {
    return state % MAC_Q_TABLE_SIZE;
}

MacAction mac_q_select_action(MacContext *ctx, MacState state) {
    uint32_t idx = mac_state_to_idx(state);
    
    if (idx >= MAC_Q_TABLE_SIZE) idx = 0;
    
    /* Epsilon-greedy */
    if (((float)rand() / RAND_MAX) < ctx->epsilon) {
        return (MacAction)(rand() % MAC_MAX_ACTIONS);
    }
    
    /* Greedy: select action with highest Q-value */
    float best_q = -1e9f;
    MacAction best_action = MAC_ACTION_WAIT;
    
    for (int a = 0; a < MAC_MAX_ACTIONS; a++) {
        float q = ctx->q_table[idx].q_values[a];
        if (q > best_q) {
            best_q = q;
            best_action = (MacAction)a;
        }
    }
    
    return best_action;
}

void mac_q_learn(MacContext *ctx, MacState state, MacAction action, 
                 float reward, MacState next_state) {
    uint32_t idx = mac_state_to_idx(state);
    uint32_t next_idx = mac_state_to_idx(next_state);
    
    if (idx >= MAC_Q_TABLE_SIZE) idx = 0;
    if (next_idx >= MAC_Q_TABLE_SIZE) next_idx = 0;
    if (action >= MAC_MAX_ACTIONS) return;
    
    /* Find max Q for next state */
    float max_next_q = -1e9f;
    for (int a = 0; a < MAC_MAX_ACTIONS; a++) {
        float q = ctx->q_table[next_idx].q_values[a];
        if (q > max_next_q) max_next_q = q;
    }
    
    /* Q-learning update: Q(s,a) = Q(s,a) + alpha * (reward + gamma * max_a' Q(s',a') - Q(s,a)) */
    float old_q = ctx->q_table[idx].q_values[action];
    float target = reward + ctx->gamma * max_next_q;
    ctx->q_table[idx].q_values[action] = old_q + ctx->alpha * (target - old_q);
    ctx->q_table[idx].visits[action]++;
    
    ctx->q_updates++;
    
    /* Decay epsilon */
    ctx->epsilon *= 0.9999f;
    if (ctx->epsilon < 0.01f) ctx->epsilon = 0.01f;
}

/* Behavior Tree implementation */
BtNode* bt_create_sequence(void) {
    BtNode *node = calloc(1, sizeof(BtNode));
    node->type = BT_NODE_SEQUENCE;
    return node;
}

BtNode* bt_create_selector(void) {
    BtNode *node = calloc(1, sizeof(BtNode));
    node->type = BT_NODE_SELECTOR;
    return node;
}

BtNode* bt_create_action(MacAction action) {
    BtNode *node = calloc(1, sizeof(BtNode));
    node->type = BT_NODE_ACTION;
    node->action = action;
    return node;
}

BtNode* bt_create_condition(bool (*cond)(void*), void *data) {
    BtNode *node = calloc(1, sizeof(BtNode));
    node->type = BT_NODE_CONDITION;
    node->condition = cond;
    node->user_data = data;
    return node;
}

void bt_add_child(BtNode *parent, BtNode *child) {
    if (parent && child && parent->child_count < 4) {
        parent->children[parent->child_count++] = child;
    }
}

MacAction bt_execute(BtNode *root, void *ctx) {
    if (!root) return MAC_ACTION_WAIT;
    
    const char *type_names[] = {"SEQUENCE", "SELECTOR", "ACTION", "CONDITION"};
    
    switch (root->type) {
        case BT_NODE_SEQUENCE: {
            MacAction last_result = MAC_ACTION_WAIT;
            for (int i = 0; i < root->child_count; i++) {
                MacAction result = bt_execute(root->children[i], ctx);
                if (result == MAC_ACTION_WAIT) return MAC_ACTION_WAIT;
                last_result = result;
            }
            return last_result;
        }
            
        case BT_NODE_SELECTOR: {
            for (int i = 0; i < root->child_count; i++) {
                MacAction result = bt_execute(root->children[i], ctx);
                if (result != MAC_ACTION_WAIT) {
                    return result;
                }
            }
            return MAC_ACTION_WAIT;
        }
            
        case BT_NODE_ACTION:
            return root->action;
            
        case BT_NODE_CONDITION: {
            bool cond_result = root->condition ? root->condition(ctx) : false;
            if (cond_result) {
                return MAC_ACTION_TX_NOW;
            }
            return MAC_ACTION_WAIT;
        }
    }
    return MAC_ACTION_WAIT;
}

/* Debug version for testing */
MacAction bt_execute_debug(BtNode *root, void *ctx, int depth) {
    if (!root) return MAC_ACTION_WAIT;
    
    const char *type_names[] = {"SEQUENCE", "SELECTOR", "ACTION", "CONDITION"};
    if (depth < 5) {
        for (int i = 0; i < depth; i++) printf("  ");
        printf("%s", type_names[root->type]);
        if (root->type == BT_NODE_ACTION) printf("(%d)", root->action);
        if (root->type == BT_NODE_CONDITION) printf("(cond)");
        printf("\n");
    }
    
    switch (root->type) {
        case BT_NODE_SEQUENCE: {
            MacAction last_result = MAC_ACTION_WAIT;
            for (int i = 0; i < root->child_count; i++) {
                MacAction result = bt_execute_debug(root->children[i], ctx, depth + 1);
                if (result == MAC_ACTION_WAIT) return MAC_ACTION_WAIT;
                last_result = result;
            }
            return last_result;
        }
            
        case BT_NODE_SELECTOR: {
            for (int i = 0; i < root->child_count; i++) {
                MacAction result = bt_execute_debug(root->children[i], ctx, depth + 1);
                if (result != MAC_ACTION_WAIT) {
                    return result;
                }
            }
            return MAC_ACTION_WAIT;
        }
            
        case BT_NODE_ACTION:
            return root->action;
            
        case BT_NODE_CONDITION: {
            bool cond_result = root->condition ? root->condition(ctx) : false;
            if (depth < 5) {
                for (int i = 0; i < depth; i++) printf("  ");
                printf("  CONDITION RESULT: %s\n", cond_result ? "TRUE" : "FALSE");
            }
            if (cond_result) {
                return MAC_ACTION_TX_NOW;
            }
            return MAC_ACTION_WAIT;
        }
    }
    return MAC_ACTION_WAIT;
}

/* Conditions for behavior tree */
static bool cond_has_queue(void *ctx) {
    MacContext *mac = (MacContext*)ctx;
    return mac->last_state == MAC_STATE_QUEUED;
}

static bool cond_channel_free(void *ctx) {
    MacContext *mac = (MacContext*)ctx;
    return mac->last_state != MAC_STATE_CHANNEL_BUSY;
}

static bool cond_my_slot(void *ctx) {
    MacContext *mac = (MacContext*)ctx;
    return mac->current_slot == mac->node_id % mac->slot_count;
}

static bool cond_collision(void *ctx) {
    MacContext *mac = (MacContext*)ctx;
    return mac->last_state == MAC_STATE_COLLISION;
}

static bool cond_near_far(void *ctx) {
    MacContext *mac = (MacContext*)ctx;
    return mac->last_state == MAC_STATE_NEAR_FAR;
}

/* Create default behavior tree for MAC */
BtNode* mac_create_default_bt(MacContext *ctx) {
    (void)ctx;
    
    /* Root: Selector (try strategies in order) */
    BtNode *root = bt_create_selector();
    
    /* Strategy 1: If collision detected, back off */
    BtNode *collision_seq = bt_create_sequence();
    bt_add_child(collision_seq, bt_create_condition(cond_collision, NULL));
    bt_add_child(collision_seq, bt_create_action(MAC_ACTION_WAIT));
    bt_add_child(root, collision_seq);
    
    /* Strategy 2: If near-far, reduce power */
    BtNode *nearfar_seq = bt_create_sequence();
    bt_add_child(nearfar_seq, bt_create_condition(cond_near_far, NULL));
    bt_add_child(nearfar_seq, bt_create_action(MAC_ACTION_POWER_LOW));
    bt_add_child(root, nearfar_seq);
    
    /* Strategy 3: If has data and channel free and my slot, transmit */
    BtNode *tx_seq = bt_create_sequence();
    bt_add_child(tx_seq, bt_create_condition(cond_has_queue, NULL));
    bt_add_child(tx_seq, bt_create_condition(cond_channel_free, NULL));
    bt_add_child(tx_seq, bt_create_condition(cond_my_slot, NULL));
    bt_add_child(tx_seq, bt_create_action(MAC_ACTION_TX_NOW));
    bt_add_child(root, tx_seq);
    
    /* Strategy 4: If has data but not my slot, wait */
    BtNode *wait_seq = bt_create_sequence();
    bt_add_child(wait_seq, bt_create_condition(cond_has_queue, NULL));
    bt_add_child(wait_seq, bt_create_action(MAC_ACTION_WAIT));
    bt_add_child(root, wait_seq);
    
    return root;
}

void mac_get_stats(const MacContext *ctx, MacStats *stats) {
    stats->tx_attempts = ctx->tx_attempts;
    stats->tx_success = ctx->tx_success;
    stats->tx_collision = ctx->tx_collision;
    stats->tx_timeout = ctx->tx_timeout;
    stats->current_epsilon = ctx->epsilon;
    stats->q_table_entries_used = 0;
    
    for (int i = 0; i < MAC_Q_TABLE_SIZE; i++) {
        for (int a = 0; a < MAC_MAX_ACTIONS; a++) {
            if (ctx->q_table[i].visits[a] > 0) {
                stats->q_table_entries_used++;
                break;
            }
        }
    }
}

void mac_reset_stats(MacContext *ctx) {
    ctx->tx_attempts = 0;
    ctx->tx_success = 0;
    ctx->tx_collision = 0;
    ctx->tx_timeout = 0;
    ctx->q_updates = 0;
    ctx->bt_decisions = 0;
}

bool mac_is_slot_boundary(const MacContext *ctx) {
    return ctx->accumulator_us == 0;
}

uint8_t mac_get_current_slot(const MacContext *ctx) {
    return ctx->current_slot;
}

uint32_t mac_time_to_next_slot(const MacContext *ctx) {
    if (ctx->accumulator_us >= ctx->fixed_dt_us) return 0;
    return ctx->fixed_dt_us - ctx->accumulator_us;
}