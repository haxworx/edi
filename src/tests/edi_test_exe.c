#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include "edi_suite.h"

START_TEST (edi_exe_test_wait)
{
   edi_init();

   ck_assert(1 != edi_exe_wait("false"));
   ck_assert_int_eq(0, edi_exe_wait("true"));

   edi_shutdown();
}
END_TEST

START_TEST (edi_exe_test_response_command_basic)
{
   char *out;
   int exit_code = -1;

   edi_init();

   out = edi_exe_response_command(NULL, "printf '%s' 'hello world'", &exit_code);
   ck_assert_ptr_nonnull(out);
   ck_assert_int_eq(0, exit_code);
   ck_assert_str_eq("hello world", out);
   free(out);

   edi_shutdown();
}
END_TEST

START_TEST (edi_exe_test_response_command_no_shell_expansion)
{
   char *out;
   int exit_code = -1;

   edi_init();

   out = edi_exe_response_command(NULL, "printf '%s' '$HOME'", &exit_code);
   ck_assert_ptr_nonnull(out);
   ck_assert_int_eq(0, exit_code);
   ck_assert_str_eq("$HOME", out);
   free(out);

   edi_shutdown();
}
END_TEST

START_TEST (edi_exe_test_response_command_invalid_quote)
{
   char *out;
   int exit_code = -1;

   edi_init();

   out = edi_exe_response_command(NULL, "printf \"unterminated", &exit_code);
   ck_assert_ptr_nonnull(out);
   ck_assert_int_eq(2, exit_code);
   ck_assert_str_eq("Error: Invalid command format.", out);
   free(out);

   edi_shutdown();
}
END_TEST

START_TEST (edi_exe_test_response_command_working_directory)
{
   char *out;
   int exit_code = -1;
   const char *tmp;

   edi_init();

   tmp = eina_environment_tmp_get();
   out = edi_exe_response_command(tmp, "pwd", &exit_code);
   ck_assert_ptr_nonnull(out);
   ck_assert_int_eq(0, exit_code);
   ck_assert(strstr(out, tmp) != NULL);
   free(out);

   edi_shutdown();
}
END_TEST

void edi_test_exe(TCase *tc)
{
   tcase_add_test(tc, edi_exe_test_wait);
   tcase_add_test(tc, edi_exe_test_response_command_basic);
   tcase_add_test(tc, edi_exe_test_response_command_no_shell_expansion);
   tcase_add_test(tc, edi_exe_test_response_command_invalid_quote);
   tcase_add_test(tc, edi_exe_test_response_command_working_directory);
}
