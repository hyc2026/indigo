#include <fstream>
#include <iostream>
#include "frontend/syntax_analyze.hpp"
#include "frontend/ir_generator.hpp"
#include "frontend/optim_mir.hpp"
#include "backend/backend.hpp"
#include "backend/codegen/codegen.hpp"
#include "backend/optimization/graph_color.hpp"
#include "mir/mir.hpp"
#include "prelude/fake_mir_generate.hpp"

using std::ifstream;
using std::ofstream;
using std::istreambuf_iterator;
using std::cout;
using std::endl;
using std::string;

const bool debug = false;

string& read_input();

int main()
{
    //frontend
    std::vector<front::word::Word> word_arr(VECTOR_SIZE);
    word_arr.clear();

    string& input_str = read_input();

    word_analyse(input_str, word_arr);
    delete& input_str;

    front::syntax::SyntaxAnalyze syntax_analyze(word_arr);
    syntax_analyze.gm_comp_unit();

    syntax_analyze.outputInstructions(std::cout);

    front::irGenerator::irGenerator& irgenerator = syntax_analyze.getIrGenerator();
    std::map<string, std::vector<front::irGenerator::Instruction>> inst = irgenerator.getfuncNameToInstructions();

    std::map<string, std::vector<front::irGenerator::Instruction>> ssa_inst = gen_ssa(inst);

    std::cout << "%%%%%%%%%%%%%%%%%%%%%%%" << std::endl;

    for (auto i : ssa_inst)
    {
        std::cout << ">====== function name : " + i.first + "======<" << std::endl;
        std::cout << ">====== vars : ======<" << std::endl;

        std::cout << ">====== instructions : ======<" << std::endl;

        for (auto j : i.second)
        {
            std::cout << "  ";
            if (j.index() == 0)
            {
                std::get<0>(j)->display(std::cout);
            }
            else if (j.index() == 1)
            {
                std::get<1>(j)->display(std::cout);
            }
            else
            {
                std::cout << "label " << std::get<2>(j)->_jumpLabelId;
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;
    }

    //backend
    front::fake::FakeGenerator x;
    x.fakeMirGenerator1();
    auto mir = x._package;
    std::cout << "MIR:" << std::endl << *mir << std::endl;
    backend::Backend backend(std::move(*mir));
    auto code = backend.generate_code();
    std::cout << "CODE:" << std::endl << code;
    return 0;

}

string& read_input()
{
    ifstream input;
    input.open("testfile.txt");
    string* input_str_p = new string(istreambuf_iterator<char>(input), istreambuf_iterator<char>());
    std::cout << *input_str_p << std::endl;
    return *input_str_p;
}