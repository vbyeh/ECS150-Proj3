//
//  temp.cpp
//  
//
//  Created by Vincent Yeh on 5/9/15.
//
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "VirtualMachine.h"
#include "Machine.h"
#define MSIZE 512


TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
{
    if ((memory == NULL) || (size = 0) || (pointer == NULL)){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if (size > MSIZE){
        return VM_Status_ERROR_INSUFFICIENT_RESOURCES;
    }
    else
        return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
{
    if((base == NULL) || (memory == NULL) || (size = 0)){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
{
    if (memory == NULL){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    return VM_STATUS_ERROR_INVALID_STATE;
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef byesleft)
{
    if ((memory == NULL) || (bytesleft == NULL)){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
{
    if ((memory == NULL) || (pointer == NULL)){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!pointer){
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else
        VM_STATUS_ERROR_INVALID_PARAMETER;
}

