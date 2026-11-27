#include "GeometryWake.h"
#include <cassert>
using osfsmp::GeometryWake;
int main(){
    GeometryWake wake;
    assert(wake.empty());
    wake.note(0); assert(wake.empty());
    wake.note(10); wake.note(20);
    assert(wake.due(10) && wake.due(20) && !wake.due(30));
    wake.scanned(10); assert(!wake.due(10) && wake.due(20));
    wake.advance(0.25f); assert(!wake.due(10));
    wake.advance(0.25f); assert(wake.due(10));
    wake.scanned(10); wake.note(10); assert(wake.due(10));
    wake.advance(1.5f); assert(wake.due(10) && !wake.due(20));
    wake.advance(0.5f); assert(wake.empty());
    wake.note(10); wake.clear(); assert(wake.empty());
}
