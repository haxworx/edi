#ifndef EDI_LISTEN_H
#define EDI_LISTEN_H

#include <Eina.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Listening and sending to existing and new Edi sessions.
 */

/**
 * @brief Listen communication functions.
 * @defgroup Listen
 *
 * @{
 *
 * Communication between Edi sessions.
 */

/**
 * Initialise and begin a listening session.
 *
 * @return Boolean success of initialisation.
 *
 * @ingroup Listen
 */
Eina_Bool
edi_listen_init();

/**
 * Shutdown the listening object and any connections, freeing the object.
 *
 * @ingroup Listen
 */
void
edi_listen_shutdown(void);

/**
 * Attempt to send a file name to an existing Edi session to open.
 *
 * @param path The path of the file to open.
 *
 * @return Boolean success of the operation.
 *
 * @ingroup Listen
 */
Eina_Bool
edi_listen_client_add(char *path);


/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif
