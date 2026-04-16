#ifndef _EDI_AGENT_PARSE_H_
# define _EDI_AGENT_PARSE_H_ 1

#include "edi_config.h"

char *edi_agent_response_parse_for_provider(const char *provider, const char *json);
char *edi_agent_error_parse_for_provider(const char *provider, const char *json, int http_code);
char *edi_agent_provider_validate_for_provider(const Edi_Project_Config *config,
                                               const char *provider_id,
                                               const char *default_endpoint,
                                               const char *default_model);

#endif
