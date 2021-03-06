
#include <utility>
#include <numeric>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <libdevcore/Common.h>
#include <libdevcore/SHA3.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/codegen/ExpressionCompiler.h>
#include <libsolidity/codegen/RuleEngineCompiler.h>
#include <libsolidity/codegen/CompilerContext.h>
#include <libsolidity/codegen/CompilerUtils.h>
#include <libsolidity/codegen/LValue.h>
#include <libsolidity/codegen/ENIHandler.h>
#include <libsolidity/codegen/DynArrUtils.h>
#include <libevmasm/GasMeter.h>

#include <libdevcore/Whiskers.h>

using namespace std;

namespace dev
{
namespace solidity
{

void RuleEngineCompiler::appendFireAllRules(ContractDefinition const& _contract)
{
	RuleEngineCompiler::appendLockRuleEngineOrFail();
	eth::AssemblyItem returnLabel = m_context.pushNewTag();
	m_context.appendJumpTo(m_context.entryFireAllRules(_contract));
	m_context << returnLabel;
	m_context.adjustStackOffset(-1);
	RuleEngineCompiler::appendUnlockRuleEngine();
}

void RuleEngineCompiler::appendFactInsert(TypePointer const& _factType)
{
	// stack pre:  itemAddr
	// stack post: factID
	// Note that in current implementation, factID = itemAddr

	// Only struct types with storage data location can be inserted
	solAssert(
		dynamic_pointer_cast<StructType const>(_factType) &&
		dynamic_pointer_cast<ReferenceType const>(_factType)->location() == DataLocation::Storage
		, "Invalid factInsert operand type"
	);

	appendAssertNoRuleEngineLock(); // fail if we are inside fireAllRules

	h256 listOfThisType = keccak256(_factType->richIdentifier()+"_ptr-factlist"); // TODO: fix type name issue

	m_context << Instruction::DUP1;
	// stack: itemAddr itemAddr

	// Save the mapping which maps "itemAddr(aka factID)" to listOfThisType
	m_context << getIdToListXorMask() << Instruction::XOR;
	// stack: itemAddr (itemAddr^idToListXorMask)
	m_context << listOfThisType << Instruction::SWAP1 << Instruction::SSTORE;

	// stack: itemAddr
	m_context << Instruction::DUP1;
	// stack: itemAddr itemAddr
	appendPushItemToStorageArray(listOfThisType);
	// stack: itemAddr listLen'
	m_context << Instruction::POP;
}

void RuleEngineCompiler::appendFactDelete()
{
	appendAssertNoRuleEngineLock(); // fail if we are inside fireAllRules

	// stack: factID
	m_context << Instruction::DUP1;
	// stack: factID factID

	// now we try to find out the listAddress which the factID stored in
	m_context << getIdToListXorMask() << Instruction::XOR << Instruction::SLOAD;
	// stack: factID listOfThisType

	appendDeleteItemInStorageArray();
}

eth::AssemblyItem RuleEngineCompiler::compileNetwork(Rule const& _rule)
{	
	eth::AssemblyItem ruleTag = m_context.newTag();
	m_context.appendJumpTo(ruleTag);
	// nodes
	_rule.accept(*this);
	// controll flow of nodes
	m_context << ruleTag;
	for(auto nodeLabel: m_nodeOrder)
	{
		eth::AssemblyItem returnLabel = m_context.pushNewTag();
		m_context.appendJumpTo(nodeLabel);
		m_context << returnLabel;
		m_context.adjustStackOffset(-1);
	}
	return ruleTag;
}

bool RuleEngineCompiler::visit(Rule const& _rule)
{
	m_currentRule = &_rule;
	return true;
}

eth::AssemblyItem RuleEngineCompiler::entryFact(FactDeclaration const& _fact)
{
	if (0 == m_entryFact.count(&_fact))
		m_entryFact.insert(make_pair(&_fact, m_context.newTag()));
	return m_entryFact.find(&_fact)->second;
}

eth::AssemblyItem RuleEngineCompiler::entryField(FieldExpression const& _field)
{
	if (0 == m_entryField.count(&_field))
		m_entryField.insert(make_pair(&_field, m_context.newTag()));
	return m_entryField.find(&_field)->second;
}

bool RuleEngineCompiler::visit(FactDeclaration const& _fact)
{
	m_context << entryFact(_fact);
	m_nodeOrder.push_back(entryFact(_fact));
	m_currentFact = &_fact;
	m_currentFieldNo = 0;
	// storage list
	auto inListAddr = keccak256(_fact.type()->richIdentifier()+"-factlist");
	// listPtr(to memList) in storage
	auto outListPtrAddr = keccak256(m_currentRule->name()+_fact.name()+"-factlist");
	m_nodeOutListPtrAddr.push_back(outListPtrAddr);
	m_context << 32*3;
	utils().allocateMemory();
	m_context << outListPtrAddr << Instruction::SSTORE;

	eth::AssemblyItem loopStart = m_context.newTag();
	eth::AssemblyItem loopEnd = m_context.newTag();

	m_context << 0 << inListAddr << Instruction::SLOAD;
	// stack: i len                                          // i=0, len
	m_context << loopStart;                                  // loop:
	// stack: i len
	m_context << Instruction::DUP2 << Instruction::DUP2;
	// stack: i len i len
	m_context << Instruction::GT << Instruction::ISZERO;     //   if i>=len
	// stack: i len !(len>i)
	m_context.appendConditionalJumpTo(loopEnd);              //     break
	// stack: i len
	m_context << inListAddr << Instruction::DUP3;
	// stack: i len inList i
	appendAccessIndexStorage();
	// stack: i len fact
	m_context << outListPtrAddr << Instruction::SLOAD;
	m_context << Instruction::SWAP1;
	// stack: listMemAddr fact
	DynArrUtils(m_context, 1).pushItem();
	// stack: i len
	m_context << Instruction::DUP2 << 1 << Instruction::ADD;  //   i++
	// stack: i len i'
	m_context << Instruction::SWAP2 << Instruction::POP;
	// stack: i' len

	m_context.appendJumpTo(loopStart);
	m_context << loopEnd;                                    // loopEnd:

	// stack: i len
	m_context << Instruction::POP << Instruction::POP;
	m_context.appendJump(eth::AssemblyItem::JumpType::OutOfFunction);
	m_context.setStackOffset(0); // not sure this is the right place
	return true;
}

bool RuleEngineCompiler::visit(FieldExpression const& _fieldExpr)
{
	// stack pre:
	// stack post:

	// Node function
	// input  : list of factID (in memory)
	// output : list of factID (in memory)
	// outline:
	//   get inList address
	//   get outList address
	//   for each fact in inList
	//     if FieldExp(the item)
	//     put this fact to outList
	m_context << entryField(_fieldExpr);
	m_nodeOrder.push_back(entryField(_fieldExpr));

	string nodeName = m_currentRule->name()+"-"+m_currentFact->name()+"-"+to_string(m_currentFieldNo);

	m_nodeOutListPtrAddr.push_back(keccak256(nodeName+"-factlist")); // TODO: dynamic allocation

	// listPtr(to memList) in storage
	auto inListPtrAddr  = m_nodeOutListPtrAddr[m_nodeOutListPtrAddr.size()-2];
	auto outListPtrAddr = m_nodeOutListPtrAddr[m_nodeOutListPtrAddr.size()-1];

	m_context << 32*3;
	utils().allocateMemory();
	m_context << outListPtrAddr << Instruction::SSTORE;

	m_context << inListPtrAddr << Instruction::SLOAD;
	DynArrUtils(m_context, 1).forEachDo(
		[&] (CompilerContext& context) {
			eth::AssemblyItem noAdd = context.newTag();
			// stack: elmtMemAddr
			// TODO: item with elementSize
			context << Instruction::MLOAD;
			// stack: fact
			context.addFact(m_currentRule->fact(0), 1);
			ExpressionCompiler(context).compile(_fieldExpr.expression());
			context.removeFact(m_currentRule->fact(0));
			// stack: fact Expr
			context << Instruction::ISZERO;
			context.appendConditionalJumpTo(noAdd);
			// stack: fact
			context << outListPtrAddr << Instruction::SLOAD;
			// stack: fact outListMemAddr
			context << Instruction::DUP2;
			// stack: fact outListMemAddr fact
			DynArrUtils(context, 1).pushItem();
			context << noAdd;
			// stack: fact
			context << Instruction::POP;
		}
	);
	m_context.appendJump(eth::AssemblyItem::JumpType::OutOfFunction);
	m_context.setStackOffset(0); // not sure this is the right place
	return false;
}

void RuleEngineCompiler::endVisit(Rule const&)
{
	m_currentRule = nullptr;
}

void RuleEngineCompiler::endVisit(FactDeclaration const&)
{
	m_currentFact = nullptr;
	m_currentFieldNo = 0;
}

void RuleEngineCompiler::endVisit(FieldExpression const&)
{
	m_currentFieldNo++;
}

CompilerUtils RuleEngineCompiler::utils()
{
	return CompilerUtils(m_context);
}

// push item to storage array (WARNING: this is not solidity dynamic array)
// listAddr is a compile-time known address
// stack: item
// stack: len'
// TODO: Take _listAddr from stack instead of from function parameter. (For more flexibility)
void RuleEngineCompiler::appendPushItemToStorageArray(h256 _listAddr)
{
	// stack: itemAddr
	m_context << _listAddr << _listAddr << Instruction::SLOAD;
	// stack: itemAddr _listAddr listLen
	m_context << 1 << Instruction::ADD;
	// stack: itemAddr _listAddr listLen'
	m_context << Instruction::DUP1 << Instruction::SWAP2;
	// stack: itemAddr listLen' listLen' _listAddr
	m_context << Instruction::SSTORE;                                 // store len
	// stack: itemAddr listLen'
	m_context << Instruction::SWAP1 << Instruction::DUP2;
	// stack: listLen' itemAddr listLen'
	m_context << _listAddr << Instruction::ADD << Instruction::SSTORE; // store item
	// stack: listLen'
}

// remove an item in storage array, by using O(N) linear search to find the item,
// then move the last item in the array to the deleted location, then decrease the array length.
// If can not find the element, call invalid instruction
// stack pre: itemValue listAddr
// stack post:
void RuleEngineCompiler::appendDeleteItemInStorageArray()
{
	eth::AssemblyItem loopStart = m_context.newTag();
	eth::AssemblyItem loopEnd = m_context.newTag();
	eth::AssemblyItem elementNotFound = m_context.newTag();

	m_context << 0;
	// stack: itemValue listAddr i                           // i=0
	m_context << loopStart;                                  // loop:
	m_context << Instruction::DUP2 << Instruction::SLOAD;

	// stack: itemValue listAddr i len
	m_context << Instruction::DUP2 << Instruction::LT;
	// stack: itemValue listAddr i (i < len)
	m_context << Instruction::ISZERO;
	// stack: itemValue listAddr i (i >= len)
	m_context.appendConditionalJumpTo(elementNotFound);
	// stack: itemValue listAddr i
	m_context << Instruction::DUP2 << Instruction::DUP2;
	// stack: itemValue listAddr i listAddr i
	appendAccessIndexStorage();
	// stack: itemValue listAddr i fact
	m_context << Instruction::DUP4 << Instruction::EQ;
	// stack: itemValue listAddr i (fact == itemValue)
	m_context.appendConditionalJumpTo(loopEnd);
	// stack: itemValue listAddr i

	m_context << 1 << Instruction::ADD;                      //   i++
	m_context.appendJumpTo(loopStart);

	m_context << elementNotFound;                            // elementNotFound:
	m_context << Instruction::INVALID;

	m_context << loopEnd;                                    // loopEnd:
	// stack: itemValue listAddr i
	m_context << Instruction::DUP2 << Instruction::SLOAD;    // TODO: Avoid SLOAD(listAddr) twice
	// stack: itemValue listAddr i len
	m_context << 1 << Instruction::SWAP1 << Instruction::SUB;
	// stack: itemValue listAddr i (len-1)
	m_context << Instruction::DUP3 << Instruction::SWAP1;
	// stack: itemValue listAddr i listAddr (len-1)
	appendAccessIndexStorage();
	// stack: itemValue listAddr i fact
	m_context << Instruction::DUP3 << Instruction::DUP3 << Instruction::DUP3;
	appendWriteIndexStorage();
	// stack: itemValue listAddr i fact
	m_context << Instruction::POP << Instruction::POP;

	// Decrease length
	// stack: itemValue listAddr
	m_context << Instruction::DUP1 << Instruction::SLOAD;
	// stack: itemValue listAddr len
	m_context << 1 << Instruction::SWAP1 << Instruction::SUB;
	// stack: itemValue listAddr (len-1)
	m_context << Instruction::SWAP1 << Instruction::SSTORE;
	// stack: itemValue
	m_context << Instruction::POP;
}

void RuleEngineCompiler::appendUpdate()
{
	// execute Instruction::INVALID if the rule engine is not executing
	appendAssertHaveRuleEngineLock();
	// In current implementation we don't care which fact is updated, so we just pop it
	m_context << Instruction::POP;
	// set reevaluation mark to true
	m_context << 1 << getRuleEngineReevaluateLocation() << Instruction::SSTORE;
}

// stack pre: array index
// stack post: item
void RuleEngineCompiler::appendAccessIndexStorage()
{
	// stack: array index
	m_context << 1 << Instruction::ADD;
	// stack: array index+1
	m_context << Instruction::ADD << Instruction::SLOAD;
	// stack: item
}

// stack pre: listAddr index value
// stack post:
void RuleEngineCompiler::appendWriteIndexStorage()
{
	m_context << Instruction::DUP2 << 1 << Instruction::ADD;
	// stack: listAddr index value (index+1)
	m_context << Instruction::DUP4 << Instruction::ADD;
	// stack: listAddr index value (listAddr+index+1)
	m_context << Instruction::SSTORE << Instruction::POP << Instruction::POP;
}

void RuleEngineCompiler::appendLockRuleEngineOrFail()
{
	m_context << getRuleEngineLockLocation() << Instruction::SLOAD;
	// stack: isLocked
	m_context << Instruction::ISZERO;
	// stack: !isLocked
	eth::AssemblyItem setLock = m_context.newTag();
	m_context.appendConditionalJumpTo(setLock);
	m_context << Instruction::INVALID; // already locked
	m_context << setLock;
	m_context << 1 << getRuleEngineLockLocation() << Instruction::SSTORE;
}

void RuleEngineCompiler::appendUnlockRuleEngine()
{
	m_context << 0 << getRuleEngineLockLocation() << Instruction::SSTORE;
}

void RuleEngineCompiler::appendAssertNoRuleEngineLock()
{
	m_context << getRuleEngineLockLocation() << Instruction::SLOAD;
	// stack: isLocked
	m_context << Instruction::ISZERO;
	// stack: !isLocked
	eth::AssemblyItem ok = m_context.newTag();
	m_context.appendConditionalJumpTo(ok);
	m_context << Instruction::INVALID; // already locked
	m_context << ok;
}

void RuleEngineCompiler::appendAssertHaveRuleEngineLock()
{
	m_context << getRuleEngineLockLocation() << Instruction::SLOAD;
	// stack: isLocked
	eth::AssemblyItem ok = m_context.newTag();
	m_context.appendConditionalJumpTo(ok);
	m_context << Instruction::INVALID; // no lock
	m_context << ok;
}

void RuleEngineCompiler::appendCleanUpNodes()
{
	for (auto ptr: m_nodeOutListPtrAddr)
	{
		m_context << ptr << Instruction::SLOAD;
		DynArrUtils(m_context, 1).clearArray();
	}
}

}
}
