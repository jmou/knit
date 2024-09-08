#pragma once

#include "object.h"

struct object* peel_spec(char* spec, size_t len);

struct resource* peel_resource(char* spec);
struct job* peel_job(char* spec);
struct production* peel_production(char* spec);
struct invocation* peel_invocation(char* spec);
