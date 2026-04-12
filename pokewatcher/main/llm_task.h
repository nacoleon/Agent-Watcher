#ifndef POKEWATCHER_LLM_TASK_H
#define POKEWATCHER_LLM_TASK_H

#define PW_LLM_ENDPOINT_LEN  128
#define PW_LLM_API_KEY_LEN   128
#define PW_LLM_MODEL_LEN     64

typedef struct {
    char endpoint[PW_LLM_ENDPOINT_LEN];
    char api_key[PW_LLM_API_KEY_LEN];
    char model[PW_LLM_MODEL_LEN];
} pw_llm_config_t;

void pw_llm_init(void);
void pw_llm_set_config(const pw_llm_config_t *config);
pw_llm_config_t pw_llm_get_config(void);
const char *pw_llm_get_last_commentary(void);
char *pw_llm_get_history_json(void);
void pw_llm_task_start(void);

#endif
