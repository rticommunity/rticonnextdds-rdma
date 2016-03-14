#ifndef QS_TEST_H
#define QS_TEST_H 

#include "reflex.h"

struct HelloWorld: public RDMA_Type
{
    unsigned int messageId;
};

REFLEX_ADAPT_STRUCT(HelloWorld,
        (unsigned int, messageId));

#endif // QS_TEST_H
