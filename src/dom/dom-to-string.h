#ifndef _AK_DOM_TO_STRING_H_
#define _AK_DOM_TO_STRING_H_

#include <string>
#include <ostream>
#include "dom/dom.h"

namespace std {

ostream& operator<< (ostream& dst, const std::pair<ltm::pin<dom::DomItem>, ltm::pin<dom::Dom>>& v);
ostream& operator<< (ostream& dst, const ltm::pin<dom::Name>& name);

string to_string(ltm::pin<dom::DomItem> root, ltm::pin<dom::Dom> dom);
string to_string(const ltm::pin<dom::Name>& name);

}  // namespace std

#endif  // _AK_DOM_TO_STRING_H_
