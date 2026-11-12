#!/usr/bin/env python3
"""Exercise the Sofia completion producer with synthetic session/handle state.
This is a unit gate, not a wire-level ACK or media test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

def extract(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2]/'src/mod/endpoints/mod_sofia/sofia.c')
    p.add_argument('--cc', default='cc')
    a = p.parse_args()
    source = a.source.read_text()
    helper = extract(source, 'static void sofia_recovery_handoff_ready(')
    # Preserve the original stack state because the callback can synthesize ss_state.
    assert 'event_state = ss_state;' in source
    assert 'sofia_recovery_handoff_ready(tech_pvt, nh, event_state, status);' in source
    fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define SWITCH_FALSE 0
#define CF_RECOVERED 1
#define nua_callstate_ready 8
#define zstr(s) (!(s) || !*(s))
typedef int nua_handle_t;
typedef struct { int alive, recovered; char token[64], ready[64]; unsigned writes; } switch_channel_t;
typedef struct { nua_handle_t *nh; switch_channel_t *channel; int sent_invites, sent_last_invite; } private_object_t;
static int switch_channel_up(switch_channel_t *c) { return c->alive; }
static int switch_channel_test_flag(switch_channel_t *c, int flag) { (void)flag; return c->recovered; }
static const char *switch_channel_get_variable_strdup(switch_channel_t *c, const char *name) {
 (void)name; return strdup(c->token);
}
static void switch_channel_set_variable(switch_channel_t *c, const char *name, const char *value) {
 if (!strcmp(name, "sip_recovery_handoff_ready")) { strcpy(c->ready,value); ++c->writes; }
 else c->token[0]=0;
}
'''+helper+r'''
int main(void) {
 switch_channel_t c;
 private_object_t p;
 nua_handle_t h, other;
 int state, status, sent, last, alive, recovered, matched;
 unsigned cases = 0;
 for (state=7;state<=9;++state)
 for (status=180;status<=201;++status)
 for (sent=0;sent<=2;++sent)
 for (last=0;last<=1;++last)
 for (alive=0;alive<=1;++alive)
 for (recovered=0;recovered<=1;++recovered)
 for (matched=0;matched<=1;++matched) {
  int expected;
  memset(&c,0,sizeof(c)); strcpy(c.token,"synthetic-attempt");
  c.alive=alive; c.recovered=recovered;
  p.nh=&h; p.channel=&c; p.sent_invites=sent; p.sent_last_invite=last;
  expected=state==8 && status==200 && sent==1 && last && alive && recovered && matched;
  sofia_recovery_handoff_ready(&p,matched?&h:&other,state,status);
  assert((c.writes==1)==expected);
  if (expected) {
   assert(!strcmp(c.ready,"synthetic-attempt")); assert(c.token[0]==0);
   sofia_recovery_handoff_ready(&p,&h,state,status); assert(c.writes==1);
  } else assert(!strcmp(c.token,"synthetic-attempt"));
  ++cases;
 }
 sofia_recovery_handoff_ready(NULL,&h,8,200);
 printf("PASS %u completion combinations and one-shot/null controls (synthetic callback state)\n",cases);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sofia-handoff-') as td:
        c = Path(td)/'test.c'; binary=Path(td)/'test'
        c.write_text(fixture)
        subprocess.run([a.cc,'-std=gnu89','-Werror=declaration-after-statement','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],check=True,timeout=60)
        return subprocess.run([str(binary)],timeout=15).returncode
if __name__=='__main__': raise SystemExit(main())
