#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include "edi_suite.h"
#include "../bin/edi_agent_parse.c"

START_TEST(edi_test_agent_parse_google_response)
{
   char *parsed;

   parsed = edi_agent_response_parse_for_provider(
      "google_codex",
      "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello world\"}]}}]}");

   ck_assert_ptr_nonnull(parsed);
   ck_assert_str_eq(parsed, "hello world");
   free(parsed);
}
END_TEST

START_TEST(edi_test_agent_parse_openai_response)
{
   char *parsed;

   parsed = edi_agent_response_parse_for_provider(
      "openai_compatible",
      "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"refactor done\"}}]}");

   ck_assert_ptr_nonnull(parsed);
   ck_assert_str_eq(parsed, "refactor done");
   free(parsed);
}
END_TEST

START_TEST(edi_test_agent_parse_openai_responses_output_text)
{
   char *parsed;

   parsed = edi_agent_response_parse_for_provider(
      "openai_compatible",
      "{\"output\":[{\"content\":[{\"type\":\"output_text\",\"text\":\"refactor done\"}]}]}");

   ck_assert_ptr_nonnull(parsed);
   ck_assert_str_eq(parsed, "refactor done");
   free(parsed);
}
END_TEST

START_TEST(edi_test_agent_parse_openai_responses_delta)
{
   char *parsed;

   parsed = edi_agent_response_parse_for_provider(
      "openai_compatible",
      "{\"type\":\"response.output_text.delta\",\"delta\":\"abc\"}");

   ck_assert_ptr_nonnull(parsed);
   ck_assert_str_eq(parsed, "abc");
   free(parsed);
}
END_TEST

START_TEST(edi_test_agent_parse_error_message)
{
   char *parsed;

   parsed = edi_agent_error_parse_for_provider(
      "openai_compatible",
      "{\"error\":{\"message\":\"bad key\",\"type\":\"invalid_request_error\",\"code\":\"401\",\"status\":\"UNAUTHENTICATED\"}}",
      401);

   ck_assert_ptr_nonnull(parsed);
   ck_assert(strstr(parsed, "HTTP 401") != NULL);
   ck_assert(strstr(parsed, "bad key") != NULL);
   ck_assert(strstr(parsed, "status=UNAUTHENTICATED") != NULL);
   free(parsed);
}
END_TEST

START_TEST(edi_test_agent_validate_rules)
{
   Edi_Project_Config cfg = {0};
   char *err;

   cfg.agent.enabled = EINA_TRUE;

   err = edi_agent_provider_validate_for_provider(&cfg, "openai_compatible",
                                                  "https://api.example.com/v1/responses",
                                                  "gpt-test");
   ck_assert_ptr_nonnull(err);
   ck_assert_str_eq(err, "API key is required for this provider.");
   free(err);

   cfg.agent.api_key = "token";
   err = edi_agent_provider_validate_for_provider(&cfg, "openai_compatible",
                                                  "https://api.example.com/v1/responses",
                                                  "gpt-test");
   ck_assert_ptr_null(err);

   cfg.agent.api_key = "";
   err = edi_agent_provider_validate_for_provider(&cfg, "local_http",
                                                  "http://127.0.0.1:11434/v1/responses",
                                                  "qwen2.5");
   ck_assert_ptr_null(err);
}
END_TEST

void
edi_test_agent_parse(TCase *tc)
{
   tcase_add_test(tc, edi_test_agent_parse_google_response);
   tcase_add_test(tc, edi_test_agent_parse_openai_response);
   tcase_add_test(tc, edi_test_agent_parse_openai_responses_output_text);
   tcase_add_test(tc, edi_test_agent_parse_openai_responses_delta);
   tcase_add_test(tc, edi_test_agent_parse_error_message);
   tcase_add_test(tc, edi_test_agent_validate_rules);
}
