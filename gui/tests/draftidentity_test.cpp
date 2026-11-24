#include "../src/draftidentity.h"
#include <iostream>
#include <QTemporaryDir>
int main() {
    using namespace draftidentity;
    int failed=0; auto check=[&](bool v,const char *m){if(!v){std::cerr<<m<<'\n';++failed;}};
    const auto a=identity("nvme.0",QJsonValue(QStringLiteral("device A"))), b=identity("nvme.0",QJsonValue(QStringLiteral("device B")));
    check(validHash(a)&&a!=b,"device identity");
    check(identity("nvme.0",QJsonValue()).isEmpty(),"no fallback to numbered slot");
    check(validHash(identity("vm.swappiness",QJsonValue())),"legacy helper supported");
    const QJsonObject saved{{"draft","8589934592"},{"live","0"},{"boot",""},{"identity",a}};
    const QJsonObject rec{{"version",2},{"fingerprint","machine"},{"values",QJsonObject{{"nvme.0",saved}}}};
    check(validRecord(rec,"machine")&&!validRecord(rec,"other"),"record identity");
    check(compatible(2,"nvme.0",saved,a)&&!compatible(2,"nvme.0",saved,b)&&!compatible(2,"nvme.0",saved,{}),"replacement and missing identity");
    check(compatible(1,"vm.swappiness",saved,{})&&!compatible(1,"nvme.0",saved,a),"v1 migration scope");
    auto invalid=rec; invalid["version"]=3;check(!validRecord(invalid,"machine"),"future schema");
    invalid=rec; invalid["values"]=QJsonObject{{"nvme.0",QJsonObject{{"draft",0},{"live","0"},{"boot",""},{"identity",a}}}};
    check(!validRecord(invalid,"machine"),"corrupt field");
    QTemporaryDir temp; check(temp.isValid(),"temporary directory");
    const auto path=temp.filePath("draft.json"); const auto missing=snapshot(path);
    check(missing && !missing->exists,"missing snapshot");
    QFile file(path); if(file.open(QIODevice::WriteOnly)) {file.write(QJsonDocument(rec).toJson(QJsonDocument::Compact));file.close();}
    const auto present=snapshot(path); check(present && present!=missing && record(present)==rec,"snapshot round trip");
    if(file.open(QIODevice::WriteOnly|QIODevice::Truncate)) {file.write("invalid");file.close();}
    check(snapshot(path)!=present && record(snapshot(path)).isEmpty(),"modified/corrupt snapshot");
    return failed?1:0;
}
