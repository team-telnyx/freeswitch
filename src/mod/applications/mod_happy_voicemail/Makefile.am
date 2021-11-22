include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_happy_voicemail

mod_LTLIBRARIES = mod_happy_voicemail.la
mod_happy_voicemail_la_SOURCES  = mod_happy_voicemail.c happy_voicemail_app.c happy_voicemail_api.c happy_voicemail_http.c happy_voicemail_json.c happy_voicemail_vm_state.c happy_voicemail_ivr.c happy_voicemail_s3.c
mod_happy_voicemail_la_CFLAGS   = $(AM_CFLAGS) -pedantic
mod_happy_voicemail_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_happy_voicemail_la_LDFLAGS  = -lcurl -lcjson -avoid-version -module -no-undefined -shared
