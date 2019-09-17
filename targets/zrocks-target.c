#include <xapp.h>
#include <xapp-media.h>
#include <zn-media.h>

int zrocks_exit (void)
{
    return xapp_exit ();
}

int zrocks_init (void)
{
    /* Add libznd media layer */
    xapp_add_media (zn_media_register);

    return xapp_init ();
}
