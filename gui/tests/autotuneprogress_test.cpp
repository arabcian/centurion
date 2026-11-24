#include "../src/autotuneprogress.h"
#include <cstdio>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); return 1; } } while(false)
int main() {
    autotuneprogress::Run run(QStringLiteral("one"));
    CHECK(!run.accept("CENTURION_EVENT {"));
    CHECK(!run.accept("CENTURION_EVENT {\"version\":1,\"run_id\":\"old\",\"sequence\":1,\"phase\":\"inputs\"}"));
    CHECK(run.accept("CENTURION_EVENT {\"version\":1,\"run_id\":\"one\",\"sequence\":1,\"phase\":\"inputs\"}") == 0);
    CHECK(!run.accept("CENTURION_STAGE model"));
    CHECK(!run.accept("CENTURION_EVENT {\"version\":1,\"run_id\":\"one\",\"sequence\":4,\"phase\":\"model\"}"));
    CHECK(run.accept("CENTURION_EVENT {\"version\":1,\"run_id\":\"one\",\"sequence\":2,\"phase\":\"model\"}") == 1);
    CHECK(!run.accept("CENTURION_EVENT {\"version\":1,\"run_id\":\"one\",\"sequence\":1,\"phase\":\"inputs\"}"));
    CHECK(!run.accept(QByteArray(8193,'x')));
    autotuneprogress::Run old(QStringLiteral("two"));
    CHECK(old.accept("CENTURION_STAGE inputs") == 0);
    CHECK(old.accept("CENTURION_STAGE model") == 1);
    CHECK(!old.accept("CENTURION_STAGE model"));
    CHECK(old.accept("CENTURION_STAGE finished") == 4);
    return 0;
}
