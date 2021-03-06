#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "backend/backend.hpp"
#include "backend/codegen/align_code.hpp"
#include "backend/codegen/bb_rearrange.hpp"
#include "backend/codegen/codegen.hpp"
#include "backend/codegen/math_opt.hpp"
#include "backend/codegen/reg_alloc.hpp"
#include "backend/optimization/algebraic_simplification.hpp"
#include "backend/optimization/block_merge.hpp"
#include "backend/optimization/cast_inst.hpp"
#include "backend/optimization/check.hpp"
#include "backend/optimization/common_expression_delete.hpp"
#include "backend/optimization/complex_dead_code_elimination.hpp"
#include "backend/optimization/const_loop_expand.hpp"
#include "backend/optimization/const_merge.hpp"
#include "backend/optimization/const_propagation.hpp"
#include "backend/optimization/cycle.hpp"
#include "backend/optimization/excess_reg_delete.hpp"
#include "backend/optimization/exit_ahead.hpp"
#include "backend/optimization/func_array_global.hpp"
#include "backend/optimization/global_expression_move.hpp"
#include "backend/optimization/global_var_to_local.hpp"
#include "backend/optimization/graph_color.hpp"
#include "backend/optimization/inline.hpp"
#include "backend/optimization/loop_unrolling.hpp"
#include "backend/optimization/memvar_propagation.hpp"
#include "backend/optimization/mla.hpp"
#include "backend/optimization/ref_count.hpp"
#include "backend/optimization/remove_dead_code.hpp"
#include "backend/optimization/remove_temp_var.hpp"
#include "backend/optimization/value_shift_collapse.hpp"
#include "backend/optimization/var_mir_fold.hpp"
#include "frontend/ir_generator.hpp"
#include "frontend/optim_mir.hpp"
#include "frontend/optimization/bmir_optimization.hpp"
#include "frontend/optimization/bmir_variable_table.hpp"
#include "frontend/optimization/scalize_fake_var_array.hpp"
#include "frontend/syntax_analyze.hpp"
#include "include/aixlog.hpp"
#include "include/argparse/argparse.hpp"
#include "mir/mir.hpp"
#include "opt.hpp"
#include "prelude/fake_mir_generate.hpp"

using std::cout;
using std::endl;
using std::ifstream;
using std::istreambuf_iterator;
using std::ofstream;
using std::string;

const bool debug = false;
Options global_options;
string read_input(std::string&);
Options parse_options(int argc, const char** argv);

extern std::string file_header;

void add_passes(backend::Backend& backend) {
  backend.add_pass(std::make_unique<optimization::sanity_check::SanityCheck>());
  backend.add_pass(
      std::make_unique<optimization::remove_temp_var::Remove_Temp_Var>());
  backend.add_pass(
      std::make_unique<optimization::const_propagation::Const_Propagation>());
  backend.add_pass(std::make_unique<optimization::var_mir_fold::VarMirFold>());
  backend.add_pass(
      std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());
  backend.add_pass(std::make_unique<optimization::inlineFunc::Inline_Func>());
  backend.add_pass(std::make_unique<optimization::mergeBlocks::Merge_Block>());
  // inside block only and remove tmp vars
  backend.add_pass(
      std::make_unique<optimization::common_expr_del::Common_Expr_Del>());

  backend.add_pass(
      std::make_unique<optimization::global_expr_move::Global_Expr_Mov>());

  // delete common exprs new created and replace not phi vars
  backend.add_pass(
      std::make_unique<optimization::common_expr_del::Common_Expr_Del>());
  backend.add_pass(
      std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());

  backend.add_pass(std::make_unique<
                   optimization::memvar_propagation::Memory_Var_Propagation>());
  backend.add_pass(std::make_unique<optimization::const_merge::Merge_Const>());
  backend.add_pass(std::make_unique<
                   optimization::memvar_propagation::Memory_Var_Propagation>());
  backend.add_pass(
      std::make_unique<optimization::const_propagation::Const_Propagation>());
  // backend.add_pass(
  //     std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());
  backend.add_pass(
      std::make_unique<optimization::loop_expand::Const_Loop_Expand>());
  // backend.add_pass(
  //     std::make_unique<optimization::loop_unrolling::Loop_Unrolling>());
  // backend.add_pass(
  //     std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());
  // backend.add_pass(
  //     std::make_unique<optimization::common_expr_del::Common_Expr_Del>());
  backend.add_pass(std::make_unique<optimization::mergeBlocks::Merge_Block>());
  backend.add_pass(
      std::make_unique<optimization::const_propagation::Const_Propagation>());
  backend.add_pass(std::make_unique<optimization::const_merge::Merge_Const>());
  backend.add_pass(
      std::make_unique<optimization::const_propagation::Const_Propagation>());
  backend.add_pass(
      std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());
  backend.add_pass(
      std::make_unique<optimization::common_expr_del::Common_Expr_Del>());

  backend.add_pass(std::make_unique<
                   optimization::memvar_propagation::Memory_Var_Propagation>());
  backend.add_pass(
      std::make_unique<optimization::const_propagation::Const_Propagation>());
  backend.add_pass(std::make_unique<optimization::const_merge::Merge_Const>());
  backend.add_pass(
      std::make_unique<optimization::const_propagation::Const_Propagation>());
  backend.add_pass(std::make_unique<optimization::cast_inst::Cast_Inst>());
  backend.add_pass(
      std::make_unique<
          optimization::memvar_propagation::Memory_Var_Propagation>(true));
  backend.add_pass(
      std::make_unique<optimization::common_expr_del::Common_Expr_Del>(true));
  backend.add_pass(
      std::make_unique<optimization::global_expr_move::Global_Expr_Mov>(true));
  backend.add_pass(
      std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());
  backend.add_pass(
      std::make_unique<
          optimization::algebraic_simplification::AlgebraicSimplification>());
  backend.add_pass(std::make_unique<
                   optimization::value_shift_collapse::ValueShiftCollapse>());
  backend.add_pass(std::make_unique<optimization::mla::MlaPass>());
  backend.add_pass(std::make_unique<backend::codegen::BasicBlkRearrange>());
  backend.add_pass(std::make_unique<
                   optimization::complex_dce::ComplexDeadCodeElimination>());
  backend.add_pass(
      std::make_unique<optimization::remove_dead_code::Remove_Dead_Code>());
  // backend.add_pass(std::make_unique<optimization::cycle::Cycle>());
  // backend.add_pass(std::make_unique<optimization::exit_ahead::Exit_Ahead>());
  backend.add_pass(std::make_unique<optimization::mergeBlocks::Merge_Block>());
  backend.add_pass(
      std::make_unique<optimization::func_array_global::Func_Array_Global>());
  backend.add_pass(std::make_unique<backend::codegen::BasicBlkRearrange>());
  // fft will error.
  // backend.add_pass(std::make_unique<
  //                  optimization::global_var_to_local::Global_Var_to_Local>());
  backend.add_pass(std::make_unique<optimization::ref_count::Ref_Count>());
  backend.add_pass(
      std::make_unique<optimization::graph_color::Graph_Color>(7, true));

  // ARM Passes
  backend.add_pass(std::make_unique<backend::codegen::MathOptimization>());
  backend.add_pass(std::make_unique<backend::codegen::RegAllocatePass>());
  backend.add_pass(std::make_unique<backend::optimization::ExcessRegDelete>());
  // backend.add_pass(std::make_unique<backend::codegen::CodeAlignOptimization>());
}

int main(int argc, const char** argv) {
  auto options = parse_options(argc, argv);

  // ***************
  options.allow_conditional_exec = true;
  // ***************

  global_options = options;

  if (options.dry_run) {
    // Only show which passes will be run
    auto pkg = mir::inst::MirPackage();
    backend::Backend backend(pkg, options);
    add_passes(backend);
    backend.show_passes(std::cout);
    return 0;
  } else {
    // Run.
    // ==== Frontend ====
    std::vector<front::word::Word> word_arr(VECTOR_SIZE);
    word_arr.clear();

    string input_str = read_input(options.in_file);

    word_analyse(input_str, word_arr);

    front::syntax::SyntaxAnalyze syntax_analyze(word_arr);
    syntax_analyze.gm_comp_unit();

    front::irGenerator::irGenerator& irgenerator =
        syntax_analyze.getIrGenerator();
    std::map<string, std::vector<front::irGenerator::Instruction>>& inst =
        irgenerator.getfuncNameToInstructions();
    mir::inst::MirPackage& package = irgenerator.getPackage();
    front::optimization::bmir_variable_table::BmirVariableTable&
        bmirVariableTable = syntax_analyze.getBmirVariableTable();

    LOG(INFO) << ("origin bmir") << std::endl;
    if (options.verbose)
      front::irGenerator::irGenerator::outputInstructions(std::cout, package,
                                                          inst);

    front::optimization::bmir_optimization::BmirOptimization bmirOptimization(
        package, bmirVariableTable, inst, options);
    bmirOptimization.add_pass(
        std::make_unique<front::optimization::scalize_fake_var_array::
                             ScalizeFakeVarArray>());
    bmirOptimization.do_bmir_optimization();

    LOG(INFO) << ("optimized bmir") << std::endl;
    if (options.verbose)
      front::irGenerator::irGenerator::outputInstructions(std::cout, package,
                                                          inst);

    LOG(INFO) << "generating SSA" << std::endl;

    gen_ssa(inst, package, irgenerator);

    // LOG(TRACE) << "Mir" << std::endl << package << std::endl;
    LOG(INFO) << ("Mir_Before") << std::endl;
    if (options.verbose) std::cout << package << std::endl;
    LOG(INFO) << ("generating ARM code") << std::endl;

    // ==== Backend ====

    backend::Backend backend(package, options);
    add_passes(backend);
    auto code = backend.generate_code();
    if (options.verbose) {
      LOG(TRACE) << "CODE:" << std::endl;
      std::cout << code;
    }

    LOG(INFO) << "writing to output file: " << options.out_file;

    ofstream output_file(options.out_file);
    output_file << file_header << std::endl << code << std::endl;
    return 0;
  }
}

string read_input(std::string& input_filename) {
  ifstream input;
  input.open(input_filename);
  auto in = std::string(istreambuf_iterator<char>(input),
                        istreambuf_iterator<char>());
  return std::move(in);
}

Options parse_options(int argc, const char** argv) {
  Options options;

  argparse::ArgumentParser parser("compiler", "0.1.0");
  parser.add_description("Compiler for SysY language, by SEGVIOL team.");

  parser.add_argument("input").help("Input file").required();
  parser.add_argument("-o", "--output")
      .help("Output file")
      .nargs(1)
      .default_value(std::string("out.s"));
  parser.add_argument("-v", "--verbose")
      .help("Set verbosity")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("-d", "--pass-diff")
      .help("Show code difference after each pass")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("-r", "--run-pass").help("Only run pass");
  parser.add_argument("-s", "--skip-pass").help("Skip pass");
  parser.add_argument("-S", "--asm")
      .help("Emit assembly code (no effect)")
      .implicit_value(true)
      .default_value(false);
  parser.add_argument("-O", "--optimize")
      .help("Optimize code (no effect)")
      .implicit_value(true)
      .default_value(false);
  parser.add_argument("-O2", "--optimize-2")
      .help("Optimize code (no effect)")
      .implicit_value(true)
      .default_value(false);
  parser.add_argument("--dry-run")
      .help("Dry run. Show the sequence of passes but does not generate code")
      .implicit_value(true)
      .default_value(false);

  try {
    parser.parse_args(argc, argv);
  } catch (const std::runtime_error& err) {
    std::cout << err.what() << std::endl;
    std::cout << parser;
    exit(0);
  }

  AixLog::Severity lvl;
  if (parser.get<bool>("--verbose")) {
    lvl = AixLog::Severity::trace;
    options.verbose = true;
  } else {
    lvl = AixLog::Severity::info;
    options.verbose = false;
  }
  AixLog::Log::init<AixLog::SinkCout>(lvl);

  options.in_file = parser.get<std::string>("input");
  options.out_file = parser.get<std::string>("--output");

  options.show_code_after_each_pass = parser.get<bool>("--pass-diff");
  options.dry_run = parser.get<bool>("--dry-run");

  if (parser.present("--run-pass")) {
    auto out = parser.get<std::string>("--run-pass");
    std::set<std::string> run_pass;
    {
      int low = 0;
      for (int i = 0; i < out.size(); i++) {
        if (out[i] == ',') {
          std::string item = out.substr(low, i);
          run_pass.insert(item);
          low = i + 1;
        }
      }
      std::string item = out.substr(low, out.size());
      run_pass.insert(item);
    }
    {
      std::stringstream pass_name;
      for (auto i = run_pass.cbegin(); i != run_pass.cend(); i++) {
        if (i != run_pass.cbegin()) pass_name << ", ";
        pass_name << *i;
      }
      LOG(INFO) << "Only running the following passes: {}" << pass_name.str()
                << "\n";
    }
    options.run_pass = {std::move(run_pass)};
  } else {
    options.run_pass = {};
  }

  if (parser.present("--skip-pass")) {
    auto out = parser.get<std::string>("--skip-pass");
    std::set<std::string> skip_pass;
    {
      int low = 0;
      for (int i = 0; i < out.size(); i++) {
        if (out[i] == ',') {
          std::string item = out.substr(low, i);
          skip_pass.insert(item);
          low = i + 1;
        }
      }
      std::string item = out.substr(low, out.size());
      skip_pass.insert(item);
    }
    {
      std::stringstream pass_name;
      for (auto i = skip_pass.cbegin(); i != skip_pass.cend(); i++) {
        if (i != skip_pass.cbegin()) pass_name << ", ";
        pass_name << *i;
      }
      LOG(INFO) << "Skipping the following passes: {}" << pass_name.str()
                << "\n";
    }
    options.skip_pass = std::move(skip_pass);
  } else {
    options.skip_pass = {};
  }

  LOG(INFO) << "input file is " << options.in_file << "\n";
  LOG(INFO) << "output file is " << options.out_file << "\n";

  return std::move(options);
}

std::string file_header =
    ".syntax unified\n\
	.eabi_attribute	67, \"2.09\"	@ Tag_conformance\n\
	.cpu	cortex-a7\n\
	.eabi_attribute	6, 10	@ Tag_CPU_arch\n\
	.eabi_attribute	7, 65	@ Tag_CPU_arch_profile\n\
	.eabi_attribute	8, 1	@ Tag_ARM_ISA_use\n\
	.eabi_attribute	9, 2	@ Tag_THUMB_ISA_use\n\
	.fpu	neon-vfpv4\n\
	.eabi_attribute	36, 1	@ Tag_FP_HP_extension\n\
	.eabi_attribute	42, 1	@ Tag_MPextension_use\n\
	.eabi_attribute	44, 2	@ Tag_DIV_use\n\
	.eabi_attribute	34, 1	@ Tag_CPU_unaligned_access\n\
	.eabi_attribute	68, 3	@ Tag_Virtualization_use\n\
	.eabi_attribute	17, 1	@ Tag_ABI_PCS_GOT_use\n\
	.eabi_attribute	20, 1	@ Tag_ABI_FP_denormal\n\
	.eabi_attribute	21, 1	@ Tag_ABI_FP_exceptions\n\
	.eabi_attribute	23, 3	@ Tag_ABI_FP_number_model\n\
	.eabi_attribute	24, 1	@ Tag_ABI_align_needed\n\
	.eabi_attribute	25, 1	@ Tag_ABI_align_preserved\n\
	.eabi_attribute	28, 1	@ Tag_ABI_VFP_args\n\
	.eabi_attribute	38, 1	@ Tag_ABI_FP_16bit_format\n\
	.eabi_attribute	18, 4	@ Tag_ABI_PCS_wchar_t\n\
	.eabi_attribute	26, 2	@ Tag_ABI_enum_size\n\
	.eabi_attribute	14, 0	@ Tag_ABI_PCS_R9_use\n\
";
