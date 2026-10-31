/* ws: command-line front end for workspaced (see wsproto.h).
 *
 *   ws status | create <name> | invite | join <code> [name] | members
 *   ws put <file> | files | cat <file> | request <file> | requests
 *   ws grant <file> <rank> | deny <file> <rank> | get <file> <dest> | sync
 */
#include "lib.h"
#include "wsclient.h"

static char line[256], outbuf[WS_MAX_FILE + 1024];

int main(int argc, char **argv) {
    long n;
    line[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) str_cat(line, " ", sizeof(line));
        str_cat(line, argv[i], sizeof(line));
    }
    n = ws_run(line, outbuf, sizeof(outbuf));
    print(outbuf);
    return n < 0 ? 1 : 0;
}
