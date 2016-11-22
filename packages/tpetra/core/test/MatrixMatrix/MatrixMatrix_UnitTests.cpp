/*
// @HEADER
// ***********************************************************************
//
//          Tpetra: Templated Linear Algebra Services Package
//                 Copyright (2008) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Michael A. Heroux (maherou@sandia.gov)
//
// ************************************************************************
// @HEADER
*/
#include <Tpetra_ConfigDefs.hpp>
#include <Tpetra_TestingUtilities.hpp>
#include <Teuchos_UnitTestHarness.hpp>

#include "TpetraExt_MatrixMatrix.hpp"
#include "Tpetra_MatrixIO.hpp"
#include "Tpetra_DefaultPlatform.hpp"
#include "Tpetra_Vector.hpp"
#include "Tpetra_CrsMatrixMultiplyOp.hpp"
#include "Tpetra_Import.hpp"
#include "Tpetra_Export.hpp"
#include "Teuchos_DefaultComm.hpp"
#include "Teuchos_CommandLineProcessor.hpp"
#include "Teuchos_XMLParameterListHelpers.hpp"
#include "Teuchos_UnitTestHarness.hpp"
#include "MatrixMarket_Tpetra.hpp"
#include "Tpetra_RowMatrixTransposer.hpp"
#include <cmath>

namespace {
  static const double defaultEpsilon = 1e-10;
  bool verbose = false;
  std::string matnamesFile;

  using Tpetra::MatrixMarket::Reader;
  using Tpetra::CrsMatrix;
  using Tpetra::CrsMatrixMultiplyOp;
  using Tpetra::DefaultPlatform;
  using Tpetra::global_size_t;
  using Tpetra::Map;
  using Tpetra::RowMatrixTransposer;
  using Tpetra::Vector;
  using Teuchos::Array;
  using Teuchos::ArrayView;
  using Teuchos::Comm;
  using Teuchos::FancyOStream;
  using Teuchos::null;
  using Teuchos::outArg;
  using Teuchos::ParameterList;
  using Teuchos::rcp;
  using Teuchos::RCP;
  using Teuchos::REDUCE_MIN;
  using Teuchos::reduceAll;
  using Teuchos::tuple;
  using std::endl;

TEUCHOS_STATIC_SETUP(){
  Teuchos::CommandLineProcessor &clp = Teuchos::UnitTestRepository::getCLP();
  clp.setOption("matnames-file", &matnamesFile,
    "A file containing a list of matricies we'll import", true);
  clp.setOption("v", "not-verbose", &verbose,
    "Whether or not to use verbose output");
}

template<class SC, class LO, class GO,class NT>
RCP<CrsMatrix<SC, LO, GO, NT> >
getIdentityMatrix (Teuchos::FancyOStream& out,
                   const Tpetra::global_size_t globalNumRows,
                   const Teuchos::RCP<const Teuchos::Comm<int> >& comm)
{
  using Teuchos::RCP;
  using std::endl;
  typedef Tpetra::CrsMatrix<SC, LO, GO, NT> Matrix_t;
  typedef Tpetra::Map<LO, GO, NT> Map_t;

  Teuchos::OSTab tab0 (out);
  out << "getIdentityMatrix" << endl;
  Teuchos::OSTab tab1 (out);

  out << "Create row Map" << endl;
  RCP<const Map_t> identityRowMap =
    Tpetra::createUniformContigMapWithNode<LO, GO, NT> (globalNumRows, comm);

  out << "Create CrsMatrix" << endl;
  RCP<Matrix_t> identityMatrix =
    Tpetra::createCrsMatrix<SC, LO, GO, NT> (identityRowMap, 1);

  out << "Fill CrsMatrix" << endl;
  Teuchos::ArrayView<const GO> gblRows = identityRowMap->getNodeElementList ();
  for (auto it = gblRows.begin (); it != gblRows.end (); ++it) {
    Teuchos::Array<GO> col (1, *it);
    Teuchos::Array<SC> val (1, Teuchos::ScalarTraits<SC>::one ());
    identityMatrix->insertGlobalValues (*it, col (), val ());
  }

  out << "Call fillComplete" << endl;
  identityMatrix->fillComplete ();

  out << "Done!" << endl;
  return identityMatrix;
}




typedef struct add_test_results_struct{
  double correctNorm;
  double computedNorm;
  double epsilon;
} add_test_results;

typedef struct mult_test_results_struct{
  double epsilon;
  double cNorm;
  double compNorm;
} mult_test_results;


template<class Matrix_t>
add_test_results regular_add_test(
    const std::string& name,
    RCP<Matrix_t > A,
    RCP<Matrix_t > B,
    bool AT,
    bool BT,
    RCP<Matrix_t > C,
    RCP<const Comm<int> > comm)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Map<LO,GO,NT> Map_t;

  add_test_results toReturn;
  toReturn.correctNorm = C->getFrobeniusNorm ();

  RCP<const Map_t > rowmap = AT ? A->getDomainMap() : A->getRowMap();
  RCP<Matrix_t> computedC = rcp( new Matrix_t(rowmap, 1));

  SC one = Teuchos::ScalarTraits<SC>::one();
  Tpetra::MatrixMatrix::Add(*A, AT, one, *B, BT, one, computedC);
  computedC->fillComplete(A->getDomainMap(), A->getRangeMap());
  toReturn.computedNorm = computedC->getFrobeniusNorm ();
  toReturn.epsilon = fabs(toReturn.correctNorm - toReturn.computedNorm);

#if 0
  Tpetra::MatrixMarket::Writer<Matrix_t>::writeSparseFile(
    name+"_calculated.mtx", computedC);
  Tpetra::MatrixMarket::Writer<Matrix_t>::writeSparseFile(
    name+"_real.mtx", C);
#endif


  return toReturn;
}

/// \brief Test the three-argument (A, B, C) version of CrsMatrix add,
///   where the output argument C is null on input.
///
/// \tparam Matrix_t A specialization of Tpetra::CrsMatrix.
template<class Matrix_t>
add_test_results
null_add_test (const Matrix_t& A,
               const Matrix_t& B,
               const bool AT,
               const bool BT,
               const Matrix_t& C,
               Teuchos::FancyOStream& out)
{
  typedef typename Matrix_t::scalar_type scalar_type;
  typedef typename Matrix_t::local_ordinal_type local_ordinal_type;
  typedef typename Matrix_t::global_ordinal_type global_ordinal_type;
  typedef typename Matrix_t::node_type NT;
  typedef Teuchos::ScalarTraits<scalar_type> STS;
  typedef Tpetra::Map<local_ordinal_type, global_ordinal_type, NT> map_type;
  typedef Tpetra::Export<local_ordinal_type, global_ordinal_type, NT> export_type;
  const scalar_type one = STS::one ();

  out << "  Computing Frobenius norm of the expected result C" << endl;
  add_test_results toReturn;
  toReturn.correctNorm = C.getFrobeniusNorm ();

  out << "  Calling 3-arg add" << endl;
  RCP<const map_type> domainMap = BT ? B.getRangeMap () : B.getDomainMap ();
  RCP<const map_type> rangeMap = BT ? B.getDomainMap () : B.getRangeMap ();
  RCP<Matrix_t> C_computed =
    Tpetra::MatrixMatrix::add (one, AT, A, one, BT, B,
                               domainMap, rangeMap, Teuchos::null);
  TEUCHOS_TEST_FOR_EXCEPTION(
    C_computed.is_null (), std::logic_error, "3-arg add returned null.");

  RCP<Matrix_t> C_exported;
  if (! C_computed->getRowMap ()->isSameAs (* (C.getRowMap ()))) {
    // Export C_computed to C's row Map, so we can compare the two.
    export_type exp (C_computed->getRowMap (), C.getRowMap ());
    C_exported =
      Tpetra::exportAndFillCompleteCrsMatrix<Matrix_t> (C_computed, exp,
                                                             C.getDomainMap (),
                                                             C.getRangeMap ());
  } else {
    C_exported = C_computed;
  }

  TEUCHOS_TEST_FOR_EXCEPTION(
    ! C_exported->getRowMap ()->isSameAs (* (C.getRowMap ())),
    std::logic_error,
    "Sorry, C_exported and C have different row Maps.");
  TEUCHOS_TEST_FOR_EXCEPTION(
    ! C_exported->getDomainMap ()->isSameAs (* (C.getDomainMap ())),
    std::logic_error,
    "Sorry, C_exported and C have different domain Maps.");
  TEUCHOS_TEST_FOR_EXCEPTION(
    ! C_exported->getRangeMap ()->isSameAs (* (C.getRangeMap ())),
    std::logic_error,
    "Sorry, C_exported and C have different range Maps.");

  toReturn.computedNorm = C_exported->getFrobeniusNorm ();
  toReturn.epsilon = STS::magnitude (toReturn.correctNorm - toReturn.computedNorm);
  return toReturn;
}


template<class Matrix_t>
add_test_results add_into_test(
    RCP<Matrix_t > A,
    RCP<Matrix_t > B,
    bool AT,
    RCP<Matrix_t > C,
    RCP<const Comm<int> > comm)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Map<LO,GO,NT> Map_t;

  add_test_results toReturn;
  toReturn.correctNorm = C->getFrobeniusNorm ();

  RCP<const Map_t > rowmap =
    AT ? A->getDomainMap () : A->getRowMap ();
  RCP<Matrix_t> computedC = rcp (new Matrix_t (rowmap, 1));
  SC one = Teuchos::ScalarTraits<SC>::one();
  Tpetra::MatrixMatrix::Add (*A, AT, one, *B, one);
  B->fillComplete ();
  toReturn.computedNorm = B->getFrobeniusNorm ();
  toReturn.epsilon = fabs (toReturn.correctNorm - toReturn.computedNorm);

  return toReturn;
}

template<class Matrix_t>
mult_test_results multiply_test_manualfc(
  const std::string& name,
  RCP<Matrix_t> A,
  RCP<Matrix_t> B,
  bool AT,
  bool BT,
  RCP<Matrix_t> C,
  RCP<const Comm<int> > comm,
  FancyOStream& out)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Map<LO,GO,NT> Map_t;
  RCP<const Map_t> map = C->getRowMap();

  RCP<Matrix_t> computedC = rcp( new Matrix_t(map, 1));

  Tpetra::MatrixMatrix::Multiply(*A, AT, *B, BT, *computedC, false);
  computedC->fillComplete(C->getDomainMap(), C->getRangeMap());

#if 0
  Tpetra::MatrixMarket::Writer<Matrix_t>::writeSparseFile(
    name+"_calculated.mtx", computedC);
  Tpetra::MatrixMarket::Writer<Matrix_t>::writeSparseFile(
    name+"_real.mtx", C);
#endif
  SC one = Teuchos::ScalarTraits<SC>::one();

  RCP<Matrix_t> diffMatrix = Tpetra::createCrsMatrix<SC,LO,GO,NT>(C->getRowMap());
  Tpetra::MatrixMatrix::Add(*C, false, -one, *computedC, false, one, diffMatrix);
  diffMatrix->fillComplete(C->getDomainMap(), C->getRangeMap());

  mult_test_results results;
  results.cNorm    = C->getFrobeniusNorm ();
  results.compNorm = diffMatrix->getFrobeniusNorm ();
  results.epsilon  = results.compNorm/results.cNorm;

  return results;
}

template<class Matrix_t>
mult_test_results multiply_test_autofc(
  const std::string& name,
  RCP<Matrix_t> A,
  RCP<Matrix_t> B,
  bool AT,
  bool BT,
  RCP<Matrix_t> C,
  RCP<const Comm<int> > comm,
  FancyOStream& out)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Map<LO,GO,NT> Map_t;
  RCP<const Map_t> map = C->getRowMap();

  RCP<Matrix_t> computedC = rcp( new Matrix_t(map, 0));
  SC one = Teuchos::ScalarTraits<SC>::one();

  Tpetra::MatrixMatrix::Multiply(*A, AT, *B, BT, *computedC, true);

#if 0
  Tpetra::MatrixMarket::Writer<Matrix_t>::writeSparseFile(
    name+"_calculated.mtx", computedC);
  Tpetra::MatrixMarket::Writer<Matrix_t>::writeSparseFile(
    name+"_real.mtx", C);
#endif

  RCP<Matrix_t> diffMatrix = Tpetra::createCrsMatrix<SC,LO,GO,NT>(C->getRowMap());
  Tpetra::MatrixMatrix::Add(*C, false, -one, *computedC, false, one, diffMatrix);
  diffMatrix->fillComplete(C->getDomainMap(), C->getRangeMap());

  mult_test_results results;
  results.cNorm    = C->getFrobeniusNorm ();
  results.compNorm = diffMatrix->getFrobeniusNorm ();
  results.epsilon  = results.compNorm/results.cNorm;

  return results;
}


template<class Matrix_t>
mult_test_results multiply_reuse_test(
  const std::string& name,
  RCP<Matrix_t> A,
  RCP<Matrix_t> B,
  bool AT,
  bool BT,
  RCP<Matrix_t> C,
  RCP<const Comm<int> > comm,
  FancyOStream& out)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Map<LO,GO,NT> Map_t;
  typedef Vector<SC,LO,GO,NT> Vector_t;

  RCP<const Map_t> map = C->getRowMap();

  // Scaling vectors
  Teuchos::Array<typename Teuchos::ScalarTraits<SC>::magnitudeType> norms(1);

  RCP<Vector_t> leftScaling  = rcp( new Vector_t(C->getRangeMap()) );
  leftScaling->randomize();
  leftScaling->norm2(norms);
  leftScaling->scale(1.0/norms[0]);

  RCP<Vector_t> rightScaling = rcp( new Vector_t(C->getDomainMap()) );
  rightScaling->randomize();
  rightScaling->norm2(norms);
  rightScaling->scale(1.0/norms[0]);

  // computedC1 = leftScaling * (op(A)*op(B)) * rightScaling
  RCP<Matrix_t> computedC1 = rcp( new Matrix_t(map, 1));
  Tpetra::MatrixMatrix::Multiply(*A, AT, *B, BT, *computedC1, false/*call_FillCompleteOnResult*/);
  computedC1->fillComplete(C->getDomainMap(), C->getRangeMap());
  computedC1->leftScale (*leftScaling);
  computedC1->rightScale(*rightScaling);

  // NOTE (mfh 05 Jun 2016) This may even be null.  It exists at this
  // point only for the syntax.
  RCP<NT> node = map->getNode ();

  // As = leftScaling * op(A) =
  //   leftScaling * A, if AT=false
  //   A*leftScaling,   if AT=true
  RCP<Matrix_t> As = A->clone(node);
  if (AT == false) As->leftScale (*leftScaling);
  else             As->rightScale(*leftScaling);

  // Bs = op(B) * rightScaling =
  //   B * rightScaling, if BT=false
  //   rightScaling*B,   if BT=true
  RCP<Matrix_t> Bs = B->clone(node);
  if (BT == false) Bs->rightScale(*rightScaling);
  else             Bs->leftScale (*rightScaling);

  // computedC2 = op(As) * op(Bs)
  RCP<Matrix_t> computedC2 = rcp( new Matrix_t(C->getCrsGraph()) );
  computedC2->fillComplete(C->getDomainMap(), C->getRangeMap());

  computedC2->resumeFill();
  Tpetra::MatrixMatrix::Multiply(*As, AT, *Bs, BT, *computedC2, true/*call_FillCompleteOnResult*/);

  // diffMatrix = computedC2 - computedC1
  SC one = Teuchos::ScalarTraits<SC>::one();
  RCP<Matrix_t> diffMatrix = Tpetra::createCrsMatrix<SC,LO,GO,NT>(C->getRowMap());
  Tpetra::MatrixMatrix::Add(*computedC1, false, -one, *computedC2, false, one, diffMatrix);
  diffMatrix->fillComplete(C->getDomainMap(), C->getRangeMap());

  mult_test_results results;
  results.cNorm    = C->getFrobeniusNorm ();
  results.compNorm = diffMatrix->getFrobeniusNorm ();
  results.epsilon  = results.compNorm/results.cNorm;

  return results;
}


template<class Matrix_t>
mult_test_results jacobi_test(
  const std::string& name,
  RCP<Matrix_t> A,
  RCP<Matrix_t> B,
  RCP<const Comm<int> > comm,
  FancyOStream& out)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Vector<SC,LO,GO,NT> Vector_t;
  typedef Map<LO,GO,NT> Map_t;
  RCP<const Map_t> map = A->getRowMap();

  SC omega=Teuchos::ScalarTraits<SC>::one();
  Vector_t Dinv(B->getRowMap());
  Dinv.putScalar(1.0);

  // Jacobi version
  RCP<Matrix_t> C = rcp(new Matrix_t(B->getRowMap(),0));
  Tpetra::MatrixMatrix::Jacobi(omega,Dinv,*A,*B,*C);

  // Multiply + Add version
  Dinv.putScalar(omega);
  RCP<Matrix_t> AB = rcp(new Matrix_t(B->getRowMap(),0));
  RCP<Matrix_t> C_check = rcp(new Matrix_t(B->getRowMap(),0));
  Tpetra::MatrixMatrix::Multiply(*A,false,*B,false,*AB);
  AB->leftScale(Dinv);
  SC one = Teuchos::ScalarTraits<SC>::one();
  Tpetra::MatrixMatrix::Add(*AB,false,-one,*B,false,one,C_check);

  // Check the difference
  Tpetra::MatrixMatrix::Add(*C, false, -one, *C_check, one);
  C_check->fillComplete(B->getDomainMap(),B->getRangeMap());

  // Error Check
  double compNorm = C_check->getFrobeniusNorm();
  double cNorm = C->getFrobeniusNorm();
  mult_test_results results;
  results.epsilon  = compNorm/cNorm;
  results.cNorm    = cNorm;
  results.compNorm = compNorm;
  return results;
}


template<class Matrix_t>
mult_test_results jacobi_reuse_test(
  const std::string& name,
  RCP<Matrix_t> A,
  RCP<Matrix_t> B,
  RCP<const Comm<int> > comm,
  FancyOStream& out)
{
  typedef typename Matrix_t::scalar_type SC;
  typedef typename Matrix_t::local_ordinal_type LO;
  typedef typename Matrix_t::global_ordinal_type GO;
  typedef typename Matrix_t::node_type NT;
  typedef Vector<SC,LO,GO,NT> Vector_t;
  typedef Map<LO,GO,NT> Map_t;

  RCP<const Map_t> map = A->getRowMap();

  // Scaling vectors
  Teuchos::Array<typename Teuchos::ScalarTraits<SC>::magnitudeType> norms(1);
  RCP<Vector_t> rightScaling = rcp( new Vector_t(B->getDomainMap()) );
  rightScaling->randomize();
  rightScaling->norm2(norms);
  rightScaling->scale(1.0/norms[0]);
  SC one = Teuchos::ScalarTraits<SC>::one();

  SC omega = one;
  Vector_t Dinv(B->getRowMap());
  Dinv.putScalar(1.0);

  // Jacobi version
  RCP<Matrix_t> computedC1 = rcp(new Matrix_t(B->getRowMap(), 0));
  Tpetra::MatrixMatrix::Jacobi(omega, Dinv, *A, *B, *computedC1);
  computedC1->rightScale(*rightScaling);

  // Bs = B * rightScaling

  // NOTE (mfh 05 Jun 2016) This may even be null.  It exists at this
  // point only for the syntax.
  RCP<NT> node = map->getNode ();
  RCP<Matrix_t> Bs = B->clone(node);
  Bs->rightScale(*rightScaling);

  // computedC2 = (I - Dinv*A)*Bs
  RCP<Matrix_t> computedC2 = rcp( new Matrix_t(computedC1->getCrsGraph()) );
  Tpetra::MatrixMatrix::Jacobi(omega, Dinv, *A, *Bs, *computedC2);

  // diffMatrix = computedC2 - computedC1

  RCP<Matrix_t> diffMatrix = Tpetra::createCrsMatrix<SC,LO,GO,NT>(computedC1->getRowMap());
  Tpetra::MatrixMatrix::Add(*computedC1, false, -one, *computedC2, false, one, diffMatrix);
  diffMatrix->fillComplete(computedC1->getDomainMap(), computedC1->getRangeMap());

  mult_test_results results;
  results.cNorm    = computedC1->getFrobeniusNorm ();
  results.compNorm = diffMatrix->getFrobeniusNorm ();
  results.epsilon  = results.compNorm/results.cNorm;

  return results;
}


TEUCHOS_UNIT_TEST_TEMPLATE_4_DECL(Tpetra_MatMat, operations_test,SC,LO, GO, NT)  {
  RCP<const Comm<int> > comm = DefaultPlatform::getDefaultPlatform().getComm();
  typedef CrsMatrix<SC,LO,GO,NT> Matrix_t;
  const int myRank = comm->getRank ();
  //const int numProcs = comm->getSize();

  // Create an output stream that prints immediately, rather than
  // waiting until the test finishes.  This lets us get debug output
  // even if an unexpected exception or crash happens.  Unfortunately,
  // Teuchos::FancyOStream does not implement operator=, so we can't
  // replace 'out' temporarily.
  RCP<FancyOStream> newOutP = (myRank == 0) ?
    Teuchos::getFancyOStream (Teuchos::rcpFromRef (std::cerr)) :
    Teuchos::getFancyOStream (rcp (new Teuchos::oblackholestream ()));
  FancyOStream& newOut = *newOutP;

  newOut << "Tpetra sparse matrix-matrix multiply: operations_test" << endl;
  Teuchos::OSTab tab1 (newOut);

  newOut << "Get parameters from XML file" << endl;
  Teuchos::RCP<Teuchos::ParameterList> matrixSystems =
    Teuchos::getParametersFromXmlFile(matnamesFile);

  for (Teuchos::ParameterList::ConstIterator it = matrixSystems->begin();
       it != matrixSystems->end();
       ++it) {
    TEUCHOS_TEST_FOR_EXCEPTION(!it->second.isList(), std::runtime_error,
      "All top-level items in the list of matrix file names must be "
      "ParameterList instances.  " << endl <<
      "Bad tag's name: " << it->first <<
      "Type name: " << it->second.getAny().typeName() <<
      endl << endl);
 
    ParameterList currentSystem = matrixSystems->sublist (it->first);
    std::string name = currentSystem.name();
    std::string A_file = currentSystem.get<std::string> ("A");
    std::string B_file = currentSystem.get<std::string> ("B");
    std::string C_file = currentSystem.get<std::string> ("C");
    const bool AT = currentSystem.get<bool> ("TransA");
    const bool BT = currentSystem.get<bool> ("TransB");
    double epsilon = currentSystem.get<double> ("epsilon", defaultEpsilon);
    std::string op = currentSystem.get<std::string> ("op");

    RCP<Matrix_t> A = Reader<Matrix_t>::readSparseFile (A_file, comm);
    RCP<Matrix_t> B = Reader<Matrix_t>::readSparseFile (B_file, comm);
    RCP<Matrix_t> C = Reader<Matrix_t>::readSparseFile (C_file, comm);

    TEUCHOS_TEST_FOR_EXCEPTION(op != "multiply" && op != "add", std::runtime_error,
      "Unrecognized Matrix Operation: " << op);

    if (op == "multiply") {
      if (verbose)
        newOut << "Running multiply test (manual FC) for " << currentSystem.name() << endl;

      mult_test_results results = multiply_test_manualfc(name, A, B, AT, BT, C, comm, newOut);

      if (verbose) {
        newOut << "Results:"     << endl;
        newOut << "\tEpsilon: "  << results.epsilon  << endl;
        newOut << "\tcNorm: "    << results.cNorm    << endl;
        newOut << "\tcompNorm: " << results.compNorm << endl;
      }
      TEST_COMPARE(results.epsilon, <, epsilon);

      if (verbose)
        newOut << "Running multiply test (auto FC) for " << currentSystem.name() << endl;

      results = multiply_test_autofc(name, A, B, AT, BT, C, comm, newOut);

      if (verbose) {
        newOut << "Results:"     << endl;
        newOut << "\tEpsilon: "  << results.epsilon  << endl;
        newOut << "\tcNorm: "    << results.cNorm    << endl;
        newOut << "\tcompNorm: " << results.compNorm << endl;
      }
      TEST_COMPARE(results.epsilon, <, epsilon);

      if (verbose)
        newOut << "Running multiply reuse test for " << currentSystem.name() << endl;

      results = multiply_reuse_test(name, A, B, AT, BT, C, comm, newOut);

      if (verbose) {
        newOut << "Results:"     << endl;
        newOut << "\tEpsilon: "  << results.epsilon  << endl;
        newOut << "\tcNorm: "    << results.cNorm    << endl;
        newOut << "\tcompNorm: " << results.compNorm << endl;
      }
      TEST_COMPARE(results.epsilon, <, epsilon);


      // Do we try Jacobi?
      if (AT == false && BT == false && A->getDomainMap()->isSameAs(*A->getRangeMap())) {
        if (verbose)
          newOut << "Running jacobi test for " << currentSystem.name() << endl;

        results = jacobi_test(name, A, B, comm, newOut);
        if (verbose) {
          newOut << "Results:"     << endl;
          newOut << "\tEpsilon: "  << results.epsilon  << endl;
          newOut << "\tcNorm: "    << results.cNorm    << endl;
          newOut << "\tcompNorm: " << results.compNorm << endl;
        }
        TEST_COMPARE(results.epsilon, <, epsilon)

        if (verbose)
          newOut << "Running jacobi reuse test for " << currentSystem.name() << endl;

        results = jacobi_reuse_test(name, A, B, comm, newOut);

        if (verbose) {
          newOut << "Results:"     << endl;
          newOut << "\tEpsilon: "  << results.epsilon  << endl;
          newOut << "\tcNorm: "    << results.cNorm    << endl;
          newOut << "\tcompNorm: " << results.compNorm << endl;
        }
        TEST_COMPARE(results.epsilon, <, epsilon)
      }
    }
    else if (op == "add") {
      if (verbose)
        newOut << "Running 3-argument add test (nonnull C on input) for "
               << currentSystem.name() << endl;

      add_test_results results = regular_add_test(name+"_add",A, B, AT, BT, C, comm);

      TEST_COMPARE(results.epsilon, <, epsilon);
      newOut << "Regular Add Test Results: " << endl;
      newOut << "\tCorrect Norm: " << results.correctNorm << endl;
      newOut << "\tComputed norm: " << results.computedNorm << endl;
      newOut << "\tEpsilon: " << results.epsilon << endl;

      // FIXME (mfh 09 May 2013) This test doesn't currently pass.  I
      // don't think anyone ever exercised the case where C is null on
      // input before.  I'm disabling this test for now until I have a
      // chance to fix that case.
      if (verbose)
        newOut << "Running 3-argument add test (null C on input) for "
               << currentSystem.name() << endl;

      TEUCHOS_TEST_FOR_EXCEPTION(A.is_null (), std::logic_error,
                                 "Before null_add_test: A is null");
      TEUCHOS_TEST_FOR_EXCEPTION(B.is_null (), std::logic_error,
                                 "Before null_add_test: B is null");
      TEUCHOS_TEST_FOR_EXCEPTION(C.is_null (), std::logic_error,
                                 "Before null_add_test: C is null");

      results = null_add_test<Matrix_t> (*A, *B, AT, BT, *C, newOut);

      TEST_COMPARE(results.epsilon, <, epsilon);
      newOut << "Null Add Test Results: " << endl;
      newOut << "\tCorrect Norm: " << results.correctNorm << endl;
      newOut << "\tComputed norm: " << results.computedNorm << endl;
      newOut << "\tEpsilon: " << results.epsilon << endl;

      B = Reader<Matrix_t >::readSparseFile(B_file, comm, false);

      if (! BT) {
        if (verbose)
          newOut << "Running 2-argument add test for "
                 << currentSystem.name() << endl;

        results = add_into_test(A,B,AT,C,comm);
        TEST_COMPARE(results.epsilon, <, epsilon)
        newOut << "Add Into Test Results: " << endl;
        newOut << "\tCorrect Norm: " << results.correctNorm << endl;
        newOut << "\tComputed norm: " << results.computedNorm << endl;
        newOut << "\tEpsilon: " << results.epsilon << endl;
      }
    }
  }

  const int lclSuccess = success ? 1 : 0;
  int gblSuccess = 0;
  reduceAll<int, int> (*comm, REDUCE_MIN, lclSuccess, outArg (gblSuccess));
  newOut << "We made it through operations_test on all processes!" << endl;
  if (gblSuccess != 1) {
    newOut << "FAILED on at least one process!" << endl;
  }
  TEST_EQUALITY_CONST( gblSuccess, 1 );
}

/*
 * This test was created at the request of Chris Siefert to verify
 * that some inexplicable behaviour in MueLu was not due to a faulty
 * assumption in the Matrix Matrix Multiply Kernel.
 * KLN 15/06/2011
 */


TEUCHOS_UNIT_TEST_TEMPLATE_4_DECL(Tpetra_MatMat, range_row_test, SC, LO, GO, NT)  {
  RCP<const Comm<int> > comm = DefaultPlatform::getDefaultPlatform().getComm();
  typedef Map<LO,GO,NT>         Map_t;
  typedef CrsMatrix<SC,LO,GO,NT> Matrix_t;

  const int myRank = comm->getRank ();
  const int numProcs = comm->getSize();

  // Create an output stream that prints immediately, rather than
  // waiting until the test finishes.  This lets us get debug output
  // even if an unexpected exception or crash happens.  Unfortunately,
  // Teuchos::FancyOStream does not implement operator=, so we can't
  // replace 'out' temporarily.
  RCP<FancyOStream> newOutP = (myRank == 0) ?
    Teuchos::getFancyOStream (Teuchos::rcpFromRef (std::cerr)) :
    Teuchos::getFancyOStream (rcp (new Teuchos::oblackholestream ()));
  FancyOStream& newOut = *newOutP;

  newOut << "Tpetra sparse matrix-matrix multiply: range row test" << endl;
  Teuchos::OSTab tab1 (newOut);

  //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  //THIS NUMBER MUST BE EVEN SO THAT WHEN I CALCULATE THE NUMBER
  //OF ROWS IN THE DOMAIN MAP I DON'T ENCOUNTER ANY
  //WEIRD RESULTS DUE TO INTEGER DIVISION
  //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  int numRowsPerProc = 4;
  int rank = comm->getRank();
  global_size_t globalNumRows = numRowsPerProc*numProcs;

  newOut << "Create identityMatrix" << endl;
  RCP<Matrix_t > identityMatrix =
    getIdentityMatrix<SC,LO,GO,NT> (newOut, globalNumRows, comm);

  // This is just to fulfill syntax requirements.  It could even be null.
  RCP<NT> node = identityMatrix->getNode ();

  newOut << "Fill identityMatrix" << endl;
//Create "B"
  Array<GO> myRows = tuple<GO>(
    rank*numRowsPerProc,
    rank*numRowsPerProc+1,
    rank*numRowsPerProc+2,
    rank*numRowsPerProc+3);
  Array<GO> rangeElements;
  if(rank == 0){
    rangeElements = tuple<GO>(
      (numProcs-1)*numRowsPerProc+1,
      (numProcs-1)*numRowsPerProc+2,
      (numProcs-1)*numRowsPerProc,
      (numProcs-1)*numRowsPerProc+3);
  }
  else{
    rangeElements = tuple<GO>(
      (rank-1)*numRowsPerProc+1,
      (rank-1)*numRowsPerProc+2,
      (rank-1)*numRowsPerProc,
      (rank-1)*numRowsPerProc+3);
  }

  newOut << "Create row, range, and domain Maps of B" << endl;
  RCP<const Map_t > bRowMap =
    Tpetra::createNonContigMapWithNode<LO,GO,NT>(myRows, comm, node);
  RCP<const Map_t > bRangeMap =
    Tpetra::createNonContigMapWithNode<LO,GO,NT>(rangeElements, comm, node);
  //We divide by 2 to make the matrix tall and "skinny"
  RCP<const Map_t > bDomainMap =
    Tpetra::createUniformContigMapWithNode<LO,GO,NT>(globalNumRows/2, comm, node);

  newOut << "Create bMatrix" << endl;
  RCP<Matrix_t > bMatrix =
    Tpetra::createCrsMatrix<SC,LO,GO,NT>(bRowMap, 1);

  newOut << "Fill bMatrix" << endl;
  {
    Teuchos::ArrayView<const GO> gblRows = bRowMap->getNodeElementList ();
    for (auto it = gblRows.begin (); it != gblRows.end (); ++it) {
      Array<GO> col(1,(*it)/2);
      Array<SC> val(1,3.0);
      bMatrix->insertGlobalValues(*it, col(), val());
    }
  }

  newOut << "Call fillComplete on bMatrix" << endl;
  bMatrix->fillComplete(bDomainMap, bRangeMap);

  newOut << "Regular I*P" << endl;
  mult_test_results results = multiply_test_autofc(
    "Different Range and Row Maps",
    identityMatrix,
    bMatrix,
    false,
    false,
    bMatrix,
    comm,
    out);
  if(verbose){
    newOut << "Results:" << endl;
    newOut << "\tEpsilon: " << results.epsilon << endl;
    newOut << "\tcNorm: " << results.cNorm << endl;
    newOut << "\tcompNorm: " << results.compNorm << endl;
  }
  TEST_COMPARE(results.epsilon, <, defaultEpsilon);

  newOut << "Create identity2" << endl;
  RCP<Matrix_t > identity2 =
    getIdentityMatrix<SC,LO,GO,NT> (newOut, globalNumRows/2, comm);

  RCP<const Map_t > bTransRowMap =
    Tpetra::createUniformContigMapWithNode<LO,GO,NT>(globalNumRows/2,comm);

  newOut << "Create and fill bTrans" << endl;
  RCP<Matrix_t> bTrans = Tpetra::createCrsMatrix<SC,LO,GO,NT>(bTransRowMap, 1);
  Array<GO> bTransRangeElements;
  if(rank == 0){
    bTransRangeElements = tuple<GO>(
      (numProcs-1)*(numRowsPerProc/2)+1,
      (numProcs-1)*(numRowsPerProc/2));
  }
  else{
    bTransRangeElements = tuple<GO>(
      (rank-1)*(numRowsPerProc/2)+1,
      (rank-1)*(numRowsPerProc/2));
  }
  newOut << bTransRangeElements << endl;

  RCP<const Map_t > bTransRangeMap =
    Tpetra::createNonContigMapWithNode<LO,GO,NT>(bTransRangeElements, comm, node);
  RCP<const Map_t > bTransDomainMap =
    Tpetra::createUniformContigMapWithNode<LO,GO,NT>(globalNumRows, comm, node);

  newOut << "Compute identity * transpose(bTrans)" << endl;
  Tpetra::MatrixMatrix::Multiply(*identity2,false,*bMatrix, true, *bTrans, false);

  newOut << "Call fillComplete on bTrans" << endl;
  bTrans->fillComplete(bTransDomainMap, bTransRangeMap);

  newOut << "Create and fill bTransTest" << endl;
  RCP<Matrix_t > bTransTest =
    Tpetra::createCrsMatrix<SC,LO,GO,NT>(bTransRowMap, 1);

  {
    Teuchos::ArrayView<const GO> gblRows = bRowMap->getNodeElementList ();
    for (auto it = gblRows.begin (); it != gblRows.end (); ++it) {
      Teuchos::Array<GO> col(1,*it);
      Teuchos::Array<SC> val(1,3.0);
      bTransTest->insertGlobalValues((*it)/2, col(), val());
    }
  }

  newOut << "Call fillComplete on bTransTest" << endl;
  bTransTest->fillComplete(bTransDomainMap, bTransRangeMap);

  // FIXME (mfh 03 May 2016) I didn't write this output message.  I
  // don't know what it means, but I'm leaving it here in case it's
  // meaningful to someone.
  newOut << "Regular I*P^T" << endl;

  RCP<Matrix_t > bTransDiff =
    Tpetra::createCrsMatrix<SC,LO,GO,NT>(bTransRowMap, 1);
  Tpetra::MatrixMatrix::Add<SC,LO,GO,NT>(*bTransTest, false, -1.0, *bTrans, false, 1.0,bTransDiff);

  newOut << "Call fillComplete on bTransDiff" << endl;
  bTransDiff->fillComplete(bTransDomainMap, bDomainMap);

  double diffNorm = bTransDiff->getFrobeniusNorm ();
  double realNorm = bTransTest->getFrobeniusNorm ();
  double calcEpsilon = diffNorm/realNorm;

  newOut << "B" << endl;

  if(verbose){
    out << "Results:" << endl;
    out << "\tEpsilon: " << calcEpsilon << endl;
    out << "\treal norm: " << realNorm << endl;
    out << "\tcompNorm: " << diffNorm << endl;
  }
  TEST_COMPARE(calcEpsilon, <, defaultEpsilon);

  const int lclSuccess = success ? 1 : 0;
  int gblSuccess = 0;
  reduceAll<int, int> (*comm, REDUCE_MIN, lclSuccess, outArg (gblSuccess));
  newOut << "We made it through range_row_test on all processes!" << endl;
  if (gblSuccess != 1) {
    newOut << "FAILED on at least one process!" << endl;
  }
  TEST_EQUALITY_CONST( gblSuccess, 1 );
}


/**
 * This test was written at the request of Chris Siefert
 * in order to verity that A^T * I produces correct results
 * when A's rowmap and rangemap are differnt.
 * KLN 23/06/2011
 */
TEUCHOS_UNIT_TEST_TEMPLATE_4_DECL(Tpetra_MatMat, ATI_range_row_test, SC, LO, GO, NT)  {
  RCP<const Comm<int> > comm = DefaultPlatform::getDefaultPlatform().getComm();
  typedef Map<LO,GO,NT>          Map_t;
  typedef CrsMatrix<SC,LO,GO,NT> Matrix_t;

  // Create an output stream that prints immediately, rather than
  // waiting until the test finishes.  This lets us get debug output
  // even if an unexpected exception or crash happens.  Unfortunately,
  // Teuchos::FancyOStream does not implement operator=, so we can't
  // replace 'out' temporarily.
  const int myRank = comm->getRank ();
  RCP<FancyOStream> newOutP = (myRank == 0) ?
    Teuchos::getFancyOStream (Teuchos::rcpFromRef (std::cerr)) :
    Teuchos::getFancyOStream (rcp (new Teuchos::oblackholestream ()));
  FancyOStream& newOut = *newOutP;

  newOut << "Tpetra sparse matrix-matrix multiply: Test A^T * I, "
    "where A's row Map and range Map differ" << endl;
  Teuchos::OSTab tab1 (newOut);

  const int numProcs = comm->getSize();
  //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  //THIS NUMBER MUST BE EVEN SO THAT WHEN I CALCULATE THE NUMBER
  //OF ROWS IN THE DOMAIN MAP I DON'T ENCOUNTER ANY
  //WEIRD RESULTS DUE TO INTEGER DIVISION
  //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  int numRowsPerProc = 4;
  const int rank = comm->getRank();
  global_size_t globalNumRows = numRowsPerProc*numProcs;

  newOut << "Create identity matrix" << endl;
  RCP<Matrix_t> identityMatrix =
    getIdentityMatrix<SC,LO,GO,NT> (newOut, globalNumRows, comm);

  // NOTE (mfh 05 Jun 2016) This may even be null.  It exists at this
  // point only for the syntax.
  RCP<NT> node = identityMatrix->getNode ();

  newOut << "Create Maps for matrix aMat" << endl;
  Array<GO> aMyRows = tuple<GO>(
    rank*numRowsPerProc,
    rank*numRowsPerProc+1,
    rank*numRowsPerProc+2,
    rank*numRowsPerProc+3);
  RCP<const Map_t> aRowMap =
    Tpetra::createNonContigMapWithNode<LO,GO,NT>(aMyRows, comm, node);
  RCP<const Map_t> aDomainMap =
    Tpetra::createUniformContigMapWithNode<LO,GO,NT>(globalNumRows/2, comm, node);
  Array<GO> aRangeElements;
  if(rank == 0){
    aRangeElements = tuple<GO>(
      (numProcs-1)*numRowsPerProc+1,
      (numProcs-1)*numRowsPerProc+2,
      (numProcs-1)*numRowsPerProc,
      (numProcs-1)*numRowsPerProc+3);
  }
  else{
    aRangeElements = tuple<GO>(
      (rank-1)*numRowsPerProc+1,
      (rank-1)*numRowsPerProc+2,
      (rank-1)*numRowsPerProc,
      (rank-1)*numRowsPerProc+3);
  }
  RCP<const Map<LO,GO,NT> > aRangeMap =
    Tpetra::createNonContigMapWithNode<LO,GO,NT>(aRangeElements, comm, node);

  newOut << "Create matrix aMat" << endl;
  RCP<Matrix_t> aMat =
    Tpetra::createCrsMatrix<SC,LO,GO,NT>(aRowMap, 1);

  newOut << "Fill matrix aMat" << endl;
  {
    Teuchos::ArrayView<const GO> gblRows = aRowMap->getNodeElementList ();
    for (auto it = gblRows.begin (); it != gblRows.end (); ++it) {
      Teuchos::Array<GO> col(1,(*it)/2);
      Teuchos::Array<SC> val(1,3.0);
      aMat->insertGlobalValues(*it, col(), val());
    }
  }
  newOut << "Call fillComplete on matrix aMat" << endl;
  aMat->fillComplete(aDomainMap, aRangeMap);

  newOut << "Create RowMatrixTransposer with aMat" << endl;
  RowMatrixTransposer<SC,LO,GO,NT> transposer (aMat);

  newOut << "Use RowMatrixTransposer to create transpose of aMat" << endl;
  RCP<Matrix_t > knownAMat = transposer.createTranspose();

  // FIXME (mfh 03 May 2016) I'm not sure what this message means, so
  // I'll leave it.
  newOut << "Regular I*P" << endl;
  mult_test_results results = multiply_test_autofc(
    "Different Range and Row Maps",
    aMat,
    identityMatrix,
    true,
    false,
    knownAMat,
    comm,
    newOut);
  if(verbose){
    newOut << "Results:" <<endl;
    newOut << "\tEpsilon: " << results.epsilon << endl;
    newOut << "\tcNorm: " << results.cNorm << endl;
    newOut << "\tcompNorm: " << results.compNorm << endl;
  }
  TEST_COMPARE(results.epsilon, <, defaultEpsilon);

  const int lclSuccess = success ? 1 : 0;
  int gblSuccess = 0;
  reduceAll<int, int> (*comm, REDUCE_MIN, lclSuccess, outArg (gblSuccess));
  newOut << "We made it through ATI_range_row_test on all processes!" << endl;
  if (gblSuccess != 1) {
    newOut << "FAILED on at least one process!" << endl;
  }
  TEST_EQUALITY_CONST( gblSuccess, 1 );
}
/*
 * This test was added at the request of Chris Siefert
 * Turns out it fails because the original algorithm in
 * EpetraExt doesn't try to handle the case where
 * A has non-unique rows in it's row map. Perhaps I will
 * come back and address that some day.
 * KLN 28/06/2011
 */
/*TEUCHOS_UNIT_TEST(Tpetra_MatMat, Multiple_row_owners){
  RCP<const Comm<int> > comm = DefaultPlatform::getDefaultPlatform().getComm();
  ParameterList defaultParameters;
  RCP<Matrix_t > A = Reader<Matrix_t >::readSparseFile("matrices/denserATa.mtx", comm);
  RCP<Matrix_t > C = Reader<Matrix_t >::readSparseFile("matrices/denserATc.mtx", comm, true, true);
  RowMatrixTransposer<double, int,int,NT> transposer(*A);
  RCP<Matrix_t> AT = transposer.createTranspose();

  RCP<Matrix_t> importedC =
    Tpetra::createCrsMatrix<double, int, int, NT>(AT->getRowMap());
  Tpetra::Import<int,int,NT> importer(C->getRowMap(), importedC->getRowMap());
  importedC->doImport(*C, importer, Tpetra::REPLACE);
  importedC->fillComplete(AT->getDomainMap(), AT->getRangeMap());

  mult_test_results results = multiply_test(
    "AT*A with multipl row owners",
    AT,
    A,
    false,
    false,
    importedC,
    comm,
    out);
  if(verbose){
    out << "Results:" <<endl;
    out << "\tEpsilon: " << results.epsilon << endl;
    out << "\tcNorm: " << results.cNorm << endl;
    out << "\tcompNorm: " << results.compNorm << endl;
  }
  TEST_COMPARE(results.epsilon, <, defaultEpsilon)

}*/


#define UNIT_TEST_GROUP_SC_LO_GO_NO( SC, LO, GO, NT )			\
  TEUCHOS_UNIT_TEST_TEMPLATE_4_INSTANT(Tpetra_MatMat, operations_test,SC, LO, GO, NT) \
  TEUCHOS_UNIT_TEST_TEMPLATE_4_INSTANT(Tpetra_MatMat, range_row_test, SC, LO, GO, NT) \
  TEUCHOS_UNIT_TEST_TEMPLATE_4_INSTANT(Tpetra_MatMat, ATI_range_row_test, SC, LO, GO, NT)


  TPETRA_ETI_MANGLING_TYPEDEFS()

  TPETRA_INSTANTIATE_SLGN_NO_ORDINAL_SCALAR( UNIT_TEST_GROUP_SC_LO_GO_NO )


  } //namespace Tpetra

