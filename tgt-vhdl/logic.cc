/*
 *  VHDL code generation for logic devices.
 *
 *  Copyright (C) 2008  Nick Gasson (nick@nickg.me.uk)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "vhdl_target.h"
#include "vhdl_element.hh"

#include <cassert>
#include <sstream>
#include <iostream>

 
/*
 * Convert the inputs of a logic gate to a binary expression.
 */
static vhdl_expr *inputs_to_expr(vhdl_scope *scope, vhdl_binop_t op,
                                 ivl_net_logic_t log)
{
   // Not always std_logic but this is probably OK since
   // the program has already been type checked
   vhdl_binop_expr *gate =
      new vhdl_binop_expr(op, vhdl_type::std_logic());
   
   int npins = ivl_logic_pins(log);
   for (int i = 1; i < npins; i++) {
      ivl_nexus_t input = ivl_logic_pin(log, i);
      gate->add_expr(nexus_to_var_ref(scope, input));
   }

   return gate;
}

/*
 * Convert a gate intput to an unary expression.
 */
static vhdl_expr *input_to_expr(vhdl_scope *scope, vhdl_unaryop_t op,
                                ivl_net_logic_t log)
{
   ivl_nexus_t input = ivl_logic_pin(log, 1);
   assert(input);

   vhdl_expr *operand = nexus_to_var_ref(scope, input);
   return new vhdl_unaryop_expr(op, operand, vhdl_type::std_logic()); 
}

static void bufif_logic(vhdl_arch *arch, ivl_net_logic_t log, bool if0)
{
   ivl_nexus_t output = ivl_logic_pin(log, 0);
   vhdl_var_ref *lhs = nexus_to_var_ref(arch->get_scope(), output);
   assert(lhs);
   
   vhdl_expr *val = nexus_to_var_ref(arch->get_scope(), ivl_logic_pin(log, 1));
   assert(val);

   vhdl_expr *sel = nexus_to_var_ref(arch->get_scope(), ivl_logic_pin(log, 2));
   assert(val);

   vhdl_expr *on = new vhdl_const_bit(if0 ? '0' : '1');
   vhdl_expr *cmp = new vhdl_binop_expr(sel, VHDL_BINOP_EQ, on, NULL);

   ivl_signal_t sig = find_signal_named(lhs->get_name(), arch->get_scope());
   char zbit;
   switch (ivl_signal_type(sig)) {
   case IVL_SIT_TRI0:
      zbit = '0';
      break;
   case IVL_SIT_TRI1:
      zbit = '1';
      break;
   case IVL_SIT_TRI:
   default:
      zbit = 'Z';
   }
   
   vhdl_const_bit *z = new vhdl_const_bit(zbit);
   vhdl_cassign_stmt *cass = new vhdl_cassign_stmt(lhs, z);
   cass->add_condition(val, cmp);

   arch->add_stmt(cass);
}

static void udp_logic(vhdl_arch *arch, ivl_net_logic_t log)
{
   ivl_udp_t udp = ivl_logic_udp(log);
   
   cout << "UDP " << ivl_udp_name(udp) << " nin="
        << ivl_udp_nin(udp) << " rows="
        << ivl_udp_rows(udp) << endl;

   if (ivl_udp_sequ(udp)) {
      error("Sequential UDP devices not supported yet");
      return;
   }

   // As with regular case statements, the expression in a
   // `with .. select' statement must be "locally static".
   // This is achieved by first combining the inputs into
   // a temporary
   
   ostringstream ss;
   ss << ivl_logic_basename(log) << "_Tmp";
   int msb = ivl_udp_nin(udp) - 1;
   vhdl_type *tmp_type = vhdl_type::std_logic_vector(msb, 0);
   vhdl_signal_decl *tmp_decl =
      new vhdl_signal_decl(ss.str().c_str(), tmp_type);
   arch->get_scope()->add_decl(tmp_decl);

   int nin = ivl_udp_nin(udp);
   vhdl_expr *tmp_rhs;
   if (nin == 1) {
      tmp_rhs = nexus_to_var_ref(arch->get_scope(), ivl_logic_pin(log, 1));
      tmp_rhs = tmp_rhs->cast(tmp_type);
   }
   else
      tmp_rhs = inputs_to_expr(arch->get_scope(), VHDL_BINOP_CONCAT, log);

   ss.str("");
   ss << "Input to " << ivl_logic_basename(log) << " "
      << ivl_udp_name(udp) << " UDP";
   tmp_decl->set_comment(ss.str());

   vhdl_var_ref *tmp_ref =
      new vhdl_var_ref(tmp_decl->get_name().c_str(), NULL);
   arch->add_stmt(new vhdl_cassign_stmt(tmp_ref, tmp_rhs));

   // Now we can implement the UDP as a `with .. select' statement
   // by reading values out of the table
   ivl_nexus_t output_nex = ivl_logic_pin(log, 0);
   vhdl_var_ref *out = nexus_to_var_ref(arch->get_scope(), output_nex);
   vhdl_with_select_stmt *ws =
      new vhdl_with_select_stmt(new vhdl_var_ref(*tmp_ref), out);
   
   int nrows = ivl_udp_rows(udp);
   for (int i = 0; i < nrows; i++) {
      const char *row = ivl_udp_row(udp, i);
      
      vhdl_expr *value = new vhdl_const_bit(row[nin]);
      vhdl_expr *cond = new vhdl_const_bits(row, nin, false);

      ws->add_condition(value, cond);
   }

   ss.str("");
   ss << "UDP " << ivl_udp_name(udp);
   ws->set_comment(ss.str());

   arch->add_stmt(ws);
}

static vhdl_expr *translate_logic_inputs(vhdl_scope *scope, ivl_net_logic_t log)
{
   switch (ivl_logic_type(log)) {
   case IVL_LO_NOT:
      return input_to_expr(scope, VHDL_UNARYOP_NOT, log);
   case IVL_LO_AND:
      return inputs_to_expr(scope, VHDL_BINOP_AND, log);
   case IVL_LO_OR:
      return inputs_to_expr(scope, VHDL_BINOP_OR, log);
   case IVL_LO_NAND:
      return inputs_to_expr(scope, VHDL_BINOP_NAND, log);
   case IVL_LO_NOR:
      return inputs_to_expr(scope, VHDL_BINOP_NOR, log);
   case IVL_LO_XOR:
      return inputs_to_expr(scope, VHDL_BINOP_XOR, log);
   case IVL_LO_XNOR:
      return inputs_to_expr(scope, VHDL_BINOP_XNOR, log);
   case IVL_LO_BUF:
   case IVL_LO_BUFZ:
      return nexus_to_var_ref(scope, ivl_logic_pin(log, 1));
   case IVL_LO_PULLUP:
      return new vhdl_const_bit('1');
   case IVL_LO_PULLDOWN:
      return new vhdl_const_bit('0');
   default:
      error("Don't know how to translate logic type = %d to expression",
            ivl_logic_type(log));
      return NULL;
   }
}

void draw_logic(vhdl_arch *arch, ivl_net_logic_t log)
{
   switch (ivl_logic_type(log)) {
   case IVL_LO_BUFIF0:
      bufif_logic(arch, log, true);
      break;
   case IVL_LO_BUFIF1:
      bufif_logic(arch, log, false);
      break;
   case IVL_LO_UDP:
      udp_logic(arch, log);
      break;
   default:
      {          
         // The output is always pin zero
         ivl_nexus_t output = ivl_logic_pin(log, 0);
         vhdl_var_ref *lhs = nexus_to_var_ref(arch->get_scope(), output);

         vhdl_expr *rhs = translate_logic_inputs(arch->get_scope(), log);
         vhdl_cassign_stmt *ass = new vhdl_cassign_stmt(lhs, rhs);
         
         ivl_expr_t delay = ivl_logic_delay(log, 1);
         if (delay)
            ass->set_after(translate_time_expr(delay));
         
         arch->add_stmt(ass);
      }
   }
}
