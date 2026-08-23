#ifndef MAC_H
#define MAC_H

#include <stdint.h>
#include <stdbool.h>

/* MAC Adaptativo - Q-learning + Behavior Trees
 * Conforme mac-adaptativo-aprendizado.md:
 * - Q-learning tabular (cabe na RAM do MCU)
 * - Estado = fila, canal_livre?, ultimo_ACK/colisao
 * - Acao = transmitir/esperar/qual_slot/potencia
 * - Recompensa = +ACK / -timeout
 * - Behavior trees / utility AI para decisao leve no MCU
 * - Fixed timestep (delta time) para slots deterministicos */

#define MAC_MAX_STATES        256
#define MAC_MAX_ACTIONS       8
#define MAC_Q_TABLE_SIZE      (MAC_MAX_STATES * MAC_MAX_ACTIONS)
#define MAC_MAX_NODES         8

typedef enum {
    MAC_ACTION_TX_NOW     = 0,  /* Transmit immediately */
    MAC_ACTION_WAIT       = 1,  /* Wait one slot */
    MAC_ACTION_TX_SLOT_0  = 2,  /* Transmit in slot 0 */
    MAC_ACTION_TX_SLOT_1  = 3,  /* Transmit in slot 1 */
    MAC_ACTION_TX_SLOT_2  = 4,  /* Transmit in slot 2 */
    MAC_ACTION_TX_SLOT_3  = 5,  /* Transmit in slot 3 */
    MAC_ACTION_POWER_LOW  = 6,  /* Reduce power */
    MAC_ACTION_POWER_HIGH = 7   /* Increase power */
} MacAction;

typedef enum {
    MAC_STATE_IDLE        = 0,  /* No queue, channel free */
    MAC_STATE_QUEUED      = 1,  /* Queue has data */
    MAC_STATE_COLLISION   = 2,  /* Last TX collided */
    MAC_STATE_ACKED       = 3,  /* Last TX acknowledged */
    MAC_STATE_NAK         = 4,  /* Last TX NAKed */
    MAC_STATE_CHANNEL_BUSY = 5, /* Channel sensed busy */
    MAC_STATE_NEAR_FAR    = 6,  /* Near-far problem detected */
    MAC_STATE_BACKOFF     = 7   /* In backoff */
} MacState;

typedef enum {
    MAC_MODE_FIXED_TDMA   = 0,  /* Fixed TDMA schedule */
    MAC_MODE_Q_LEARNING   = 1,  /* Q-learning adaptive */
    MAC_MODE_BEHAVIOR_TREE = 2, /* Behavior tree decision */
    MAC_MODE_HYBRID       = 3   /* Q-learning with BT fallback */
} MacMode;

/* Q-learning table entry */
typedef struct {
    float q_values[MAC_MAX_ACTIONS];
    uint32_t visits[MAC_MAX_ACTIONS];
} MacQEntry;

/* Behavior Tree node types */
typedef enum {
    BT_NODE_SEQUENCE = 0,
    BT_NODE_SELECTOR = 1,
    BT_NODE_ACTION   = 2,
    BT_NODE_CONDITION = 3
} BtNodeType;

typedef struct BtNode_s {
    BtNodeType type;
    MacAction action;           /* For action nodes */
    bool (*condition)(void*);   /* For condition nodes */
    struct BtNode_s *children[4];
    uint8_t child_count;
    void *user_data;
} BtNode;

/* MAC context */
typedef struct {
    MacMode mode;
    uint8_t node_id;
    uint8_t num_nodes;
    uint8_t slot_count;        /* TDMA slots per frame */
    uint8_t current_slot;
    uint32_t slot_duration_us;
    uint32_t frame_start_time;
    
    /* Q-learning */
    MacQEntry q_table[MAC_Q_TABLE_SIZE];
    float alpha;               /* Learning rate */
    float gamma;               /* Discount factor */
    float epsilon;             /* Exploration rate */
    MacState last_state;
    MacAction last_action;
    uint32_t last_state_idx;
    
    /* Behavior Tree */
    BtNode *bt_root;
    
    /* Fixed timestep (game loop style) */
    uint32_t accumulator_us;
    uint32_t fixed_dt_us;
    
    /* Statistics */
    uint64_t tx_attempts;
    uint64_t tx_success;
    uint64_t tx_collision;
    uint64_t tx_timeout;
    uint64_t q_updates;
    uint64_t bt_decisions;
    
    uint32_t (*get_time_us)(void);
} MacContext;

/* Initialize MAC */
void mac_init(MacContext *ctx, uint8_t node_id, uint8_t num_nodes, 
              uint8_t slot_count, uint32_t slot_duration_us,
              uint32_t (*get_time_us)(void));

/* Set mode */
void mac_set_mode(MacContext *ctx, MacMode mode);

/* Game loop style fixed timestep update */
void mac_update(MacContext *ctx, uint32_t dt_us);

/* Called when channel state changes */
void mac_on_channel_busy(MacContext *ctx, bool busy);
void mac_on_tx_result(MacContext *ctx, bool success, bool collision, bool nak);
void mac_on_queue_change(MacContext *ctx, bool has_data);

/* Get next action (call at slot boundary) */
MacAction mac_get_action(MacContext *ctx);

/* Q-learning API */
void mac_q_learn(MacContext *ctx, MacState state, MacAction action, float reward, MacState next_state);
uint32_t mac_state_to_idx(MacState state);
MacAction mac_q_select_action(MacContext *ctx, MacState state);

/* Behavior Tree API */
BtNode* bt_create_sequence(void);
BtNode* bt_create_selector(void);
BtNode* bt_create_action(MacAction action);
BtNode* bt_create_condition(bool (*cond)(void*), void *data);
void bt_add_child(BtNode *parent, BtNode *child);
MacAction bt_execute(BtNode *root, void *ctx);
MacAction bt_execute_debug(BtNode *root, void *ctx, int depth);

/* Default behavior tree for MAC decisions */
BtNode* mac_create_default_bt(MacContext *ctx);

/* Statistics */
typedef struct {
    uint64_t tx_attempts;
    uint64_t tx_success;
    uint64_t tx_collision;
    uint64_t tx_timeout;
    float    current_epsilon;
    uint32_t q_table_entries_used;
} MacStats;

void mac_get_stats(const MacContext *ctx, MacStats *stats);
void mac_reset_stats(MacContext *ctx);

/* Slot timing */
bool mac_is_slot_boundary(const MacContext *ctx);
uint8_t mac_get_current_slot(const MacContext *ctx);
uint32_t mac_time_to_next_slot(const MacContext *ctx);

#endif