/*
 * AI-assisted feature note:
 * This public agent API was introduced for AI-assisted features and is
 * maintained as part of EDI.
 */
#ifndef _EDI_AGENT_H_
# define _EDI_AGENT_H_ 1

#include <Eina.h>

#include "edi_config.h"

typedef struct _Edi_Agent_Request Edi_Agent_Request;
typedef struct _Edi_Agent_Models_Request Edi_Agent_Models_Request;

typedef struct _Edi_Agent_Provider
{
   /* Stable provider identifier used in config and routing logic. */
   const char *id;
   const char *name;
   const char *default_endpoint;
   const char *default_model;
} Edi_Agent_Provider;

typedef struct _Edi_Agent_Model_List
{
   const char **models;
   unsigned int count;
} Edi_Agent_Model_List;

typedef void (*Edi_Agent_Response_Cb)(const char *response, const char *error, void *data);
/* Token callback is used for streaming providers (SSE/chunked text). */
typedef void (*Edi_Agent_Token_Cb)(const char *token, void *data);
/* Model callback may return remote models or static fallback models. */
typedef void (*Edi_Agent_Models_Cb)(const char *provider_id,
                                    const char **models,
                                    unsigned int count,
                                    Eina_Bool from_remote,
                                    const char *error,
                                    void *data);

const Edi_Agent_Provider *edi_agent_providers_get(unsigned int *count);
const Edi_Agent_Provider *edi_agent_provider_by_id(const char *id);
const Edi_Agent_Provider *edi_agent_provider_current_get(void);
Edi_Agent_Model_List edi_agent_provider_models_get(const char *provider_id);
Eina_Bool edi_agent_provider_model_supported(const char *provider_id, const char *model);
const char *edi_agent_provider_model_default_get(const char *provider_id);
Edi_Agent_Models_Request *edi_agent_provider_models_fetch(const Edi_Project_Config *config,
                                                          const char *provider_id,
                                                          Edi_Agent_Models_Cb cb,
                                                          void *data);
Eina_Bool edi_agent_provider_models_fetch_cancel(Edi_Agent_Models_Request *request);
void edi_agent_models_free(const char **models, unsigned int count);
Eina_Bool edi_agent_provider_configured_get(const Edi_Project_Config *config);
char *edi_agent_provider_validate(const Edi_Project_Config *config);

Edi_Agent_Request *edi_agent_request_send(const char *prompt, Edi_Agent_Response_Cb cb, void *data);
Edi_Agent_Request *edi_agent_request_send_stream(const char *prompt, Edi_Agent_Token_Cb token_cb,
                                                 Edi_Agent_Response_Cb done_cb, void *data);
Eina_Bool edi_agent_request_cancel(Edi_Agent_Request *request);

char *edi_agent_response_parse_for_provider(const char *provider, const char *json);
char *edi_agent_error_parse_for_provider(const char *provider, const char *json, int http_code);

#endif
