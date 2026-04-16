#ifndef __EDI_SCM_UI_H__
#define __EDI_SCM_UI_H__

#include <Elementary.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief These routines used for managing Edi SCM UI actions.
 */

typedef struct _Edi_Scm_Ui_Opts {
   Eina_Bool log;
   Eina_Bool diff;
   Eina_Bool commit;
   const char *diff_path;
   const char *show_commit;
} Edi_Scm_Ui_Opts;

/**
 * @brief SCM management functions.
 * @defgroup SCM
 *
 * @{
 *
 * Management of SCM UI actions. 
 *
 */

/**
 * Create the commit dialog UI.
 * 
 * @param parent Parent object to add the commit UI to.
 * @ingroup SCM
 */
void edi_scm_ui_add(Evas_Object *parent, Edi_Scm_Ui_Opts opts);
/**
 * @}
 */


#ifdef __cplusplus
}
#endif



#endif
