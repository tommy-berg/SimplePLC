#pragma once
#include "st/diagnostics.h"
#include "st/program.h"
#include "vm/image.h"

namespace simpleplc::st {

/**
 * Generates the image of an analysed program (st/sema.h): code for the PROGRAM
 * and every POU it calls, the data block layout with initial values, the
 * variable table, tag bindings and the line table. Expects a program without
 * errors; reports only limits (E_LIMIT).
 */
bool generate(Program& program, vm::Image& image, Diagnostics& diags);

/// Initial bytes of a variable of type `t` with initializer `init` (null: the type's default).
void writeInitialValue(uint8_t* dst, const Type* t, const ast::Initializer* init);

}  // namespace simpleplc::st
