/*
 * AI-assisted feature note:
 * This AI panel interface was introduced as part of AI-assisted feature work
 * and integrated into EDI by maintainers.
 */
#ifndef EDI_AIPANEL_H_
# define EDI_AIPANEL_H_

#include <Elementary.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates and attaches the AI panel UI to the provided parent container. */
void edi_aipanel_add(Evas_Object *parent);

#ifdef __cplusplus
}
#endif

#endif
