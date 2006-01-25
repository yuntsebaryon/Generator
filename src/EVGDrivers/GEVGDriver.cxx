//____________________________________________________________________________
/*!

\class   genie::GEVGDriver

\brief   Minimal interface object for generating neutrino interactions for
         a given initial state.

         When GMC is used, a GEVGDriver object list is assembled for all possible
         initial states (corresponding to combinations of all neutrino types
         -declared by the input GFluxI- and all target nuclei types -found
         in the input ROOT geometry-.

\author  Costas Andreopoulos <C.V.Andreopoulos@rl.ac.uk>
         CCLRC, Rutherford Appleton Laboratory

\created August 06, 2004

*/
//____________________________________________________________________________

#include <cassert>
#include <sstream>

#include <TSystem.h>
#include <TMath.h>

#include "Algorithm/AlgFactory.h"
#include "Base/XSecAlgorithmI.h"
#include "Conventions/Controls.h"
#include "Conventions/Units.h"
#include "EVGDrivers/GEVGDriver.h"
#include "EVGCore/EventRecord.h"
#include "EVGCore/EventGeneratorList.h"
#include "EVGCore/EGResponsibilityChain.h"
#include "EVGCore/EventGeneratorI.h"
#include "EVGCore/ToyInteractionSelector.h"
#include "EVGCore/PhysInteractionSelector.h"
#include "EVGCore/EventGeneratorListAssembler.h"
#include "EVGCore/InteractionList.h"
#include "EVGCore/InteractionListGeneratorI.h"
#include "EVGCore/XSecAlgorithmMap.h"
#include "Interaction/Interaction.h"
#include "Messenger/Messenger.h"
#include "Numerical/Spline.h"
#include "PDG/PDGCodes.h"
#include "PDG/PDGUtils.h"
#include "PDG/PDGLibrary.h"
#include "Utils/XSecSplineList.h"
#include "Utils/PrintUtils.h"

using std::ostringstream;

using namespace genie;
using namespace genie::controls;

//____________________________________________________________________________
namespace genie {
 ostream & operator<< (ostream& stream, const GEVGDriver & driver)
 {
   driver.Print(stream);
   return stream;
 }
}
//___________________________________________________________________________
GEVGDriver::GEVGDriver()
{
  this->Init();
}
//___________________________________________________________________________
GEVGDriver::~GEVGDriver()
{
  this->CleanUp();
}
//___________________________________________________________________________
void GEVGDriver::FilterUnphysical(bool on_off)
{
  LOG("GEVGDriver", pNOTICE)
        << "*** Filtering unphysical events is turned "
                     << utils::print::BoolAsIOString(on_off) << " ***\n";
  fFilterUnphysical = on_off;
}
//___________________________________________________________________________
void GEVGDriver::Init(void)
{
  // initial state for which this driver is configured
  fInitState = 0;
  // current event record (ownership is transfered at GenerateEvent())
  fCurrentRecord = 0;
  // list of Event Generator objects loaded into the driver
  fEvGenList = 0;
  // interaction selector
  fIntSelector = 0;
  // chain of responsibility - for selecting the Event Generator object that
  // can generate the selected interaction
  fChain = 0;
  // flag instructing the driver whether, for each interaction, to compute
  // cross section by running their corresponding XSecAlgorithm or by evaluating
  // their corresponding xsec spline
  fUseSplines = false;
  // 'depth' counter when entering a recursive mode to re-generate a failed/
  // unphysical event - the driver is not allowed to go into arbitrarily large
  // depths
  fNRecLevel = 0;
  // an "interaction" -> "xsec algorithm" associative contained built for all
  // simulated interactions (from the loaded Event Generators and for the input
  // initial state)
  fXSecAlgorithmMap = 0;
  // A spline describing the sum of all interaction cross sections given an
  // initial state (the init state with which this driver was configured).
  // Create it using the CreateXSecSumSpline() method
  // The sum of all interaction cross sections is used, for example, by
  // GMCJDriver for selecting an initial state.
  fXSecSumSpl = 0;
  // Default driver behaviour is to filter out unphysical events,
  // Set this to false to get them if needed, but be warned that the event
  // record for unphysical events might be incomplete depending on the
  // processing step that event generation was stopped.
  fFilterUnphysical = true;
}
//___________________________________________________________________________
void GEVGDriver::CleanUp(void)
{
  if (fInitState)        delete fInitState;
  if (fEvGenList)        delete fEvGenList;
  if (fIntSelector)      delete fIntSelector;
  if (fChain)            delete fChain;
  if (fXSecAlgorithmMap) delete fXSecAlgorithmMap;
  if (fXSecSumSpl)       delete fXSecSumSpl;
}
//___________________________________________________________________________
void GEVGDriver::Reset(void)
{
  this->CleanUp();
  this->Init();
}
//___________________________________________________________________________
void GEVGDriver::Configure(int nu_pdgc, int Z, int A)
{
  Target target(Z, A);
  InitialState init_state(target, nu_pdgc);

  this->Configure(init_state);
}
//___________________________________________________________________________
void GEVGDriver::Configure(const InitialState & init_state)
{
  LOG("GEVGDriver", pNOTICE) << "Configuring a GEVGDriver object";

  this -> BuildInitialState        (init_state);
  this -> BuildGeneratorList       ();
  this -> BuildXSecAlgorithmMap    ();
  this -> BuildResponsibilityChain ();
  this -> BuildInteractionSelector ();
}
//___________________________________________________________________________
void GEVGDriver::BuildInitialState(const InitialState & init_state)
{
  LOG("GEVGDriver", pNOTICE) << "Building the `InitialState`";

  if(fInitState) delete fInitState;
  fInitState = new InitialState(init_state);

  this->AssertIsValidInitState();
}
//___________________________________________________________________________
void GEVGDriver::BuildGeneratorList(void)
{
//! figure out which list of event generators to use from the $GEVGL
//! environmental variable (use "Default") if the variable is not set.

  LOG("GEVGDriver", pNOTICE) << "Building the `EventGeneratorList`";

  string evgl = (gSystem->Getenv("GEVGL") ?
                               gSystem->Getenv("GEVGL") : "Default");
  LOG("GEVGDriver", pNOTICE)
                       << "Specified Event Generator List = " << evgl;

  EventGeneratorListAssembler evglist_assembler(evgl.c_str());
  fEvGenList = evglist_assembler.AssembleGeneratorList();
}
//___________________________________________________________________________
void GEVGDriver::BuildXSecAlgorithmMap(void)
{
//! figure out which list of event generators to use from the $GEVGL
//! environmental variable (use "Default") if the variable is not set.

  LOG("GEVGDriver", pNOTICE)
     << "Building the `XSecAlgorithmMap` for init-state = " 
                                                  << fInitState->AsString();
  fXSecAlgorithmMap = new XSecAlgorithmMap;
  fXSecAlgorithmMap->UseGeneratorList(fEvGenList);
  fXSecAlgorithmMap->BuildMap(*fInitState);

  LLOG("GEVGDriver", pNOTICE) << *fXSecAlgorithmMap;
}
//___________________________________________________________________________
void GEVGDriver::BuildResponsibilityChain(void)
{
  LOG("GEVGDriver", pNOTICE)
               << "Building the `Generator Chain of Responsibility`";

  if(fChain) delete fChain;
  fChain = new EGResponsibilityChain;
  fChain->SetGeneratorList(fEvGenList);
}
//___________________________________________________________________________
void GEVGDriver::BuildInteractionSelector(void)
{
  LOG("GEVGDriver", pNOTICE) << "Building an `Interaction Selector`";

  if(fIntSelector) delete fIntSelector;
  fIntSelector = new PhysInteractionSelector("Default");
}
//___________________________________________________________________________
EventRecord * GEVGDriver::GenerateEvent(const TLorentzVector & nu4p)
{
  //-- Build initial state information from inputs
  LOG("GEVGDriver", pINFO) << "Creating the initial state";
  InitialState init_state(*fInitState);
  init_state.SetProbeP4(nu4p);

  //-- Select the interaction to be generated (amongst the entries of the
  //   InteractionList assembled by the EventGenerators) and bootstrap the
  //   event record
  LOG("GEVGDriver", pINFO)
               << "Selecting an Interaction & Bootstraping the EventRecord";
  fCurrentRecord = fIntSelector->SelectInteraction(fXSecAlgorithmMap, nu4p);

  assert(fCurrentRecord); // abort if no interaction could be selected!

  //-- Get a ptr to the interaction summary
  LOG("GEVGDriver", pDEBUG) << "Getting the selected interaction";
  Interaction * interaction = fCurrentRecord->GetInteraction();

  //-- Find the appropriate concrete EventGeneratorI implementation
  //   for generating this event.
  //
  //   The right EventGeneratorI will be selecting by iterating over the
  //   entries of the EventGeneratorList and compare the interaction
  //   against the ValidityContext declared by each EventGeneratorI
  //
  //   (note: use of the 'Chain of Responsibility' Design Pattern)

  LOG("GEVGDriver", pINFO) << "Finding an appropriate EventGenerator";

  const EventGeneratorI * evgen = fChain->FindGenerator(interaction);
  assert(evgen);

  //-- Generate the selected event
  //
  //   The selected EventGeneratorI subclass will start processing the
  //   event record (by sequentially asking each entry in its list of
  //   EventRecordVisitorI subclasses to visit and process the record).
  //   Most of the actual event generation takes place in this step.
  //
  //   (note: use of the 'Visitor' Design Pattern)

  LOG("GEVGDriver", pINFO) << "Generating Event:";

  evgen->ProcessEventRecord(fCurrentRecord);

  //-- If the user requested that unphysical events should be returned too,
  //   return the event record here
  if (!fFilterUnphysical) return fCurrentRecord;

  //-- Check whether the generated event is unphysical (eg Pauli-Blocked,
  //   etc). If the unphysical it will not be returned. This method would
  //   clean-up the failed event and it will enter into a recursive mode,
  //   calling itself so as to regenerate the event. It will exit the
  //   recursive mode when a physical event is generated
  bool unphys = fCurrentRecord->IsUnphysical();
  if(unphys) {
     LOG("GEVGDriver", pWARN) << "I generated an unphysical event!";
     delete fCurrentRecord;
     fCurrentRecord = 0;
     fNRecLevel++; // increase the nested level counter

     if(fNRecLevel<=kRecursiveModeMaxDepth) {
         LOG("GEVGDriver", pWARN) << "Attempting to regenerate the event.";
         return this->GenerateEvent(nu4p);
     } else {
        LOG("GEVGDriver", pFATAL)
             << "Could not produce a physical event after "
                      << kRecursiveModeMaxDepth << " attempts - Aborting!";
        assert(false);
     }
  }
  fNRecLevel = 0;

  //-- Return a successfully generated event to the user
  return fCurrentRecord; // The client 'adopts' the event record
}
//___________________________________________________________________________
double GEVGDriver::XSecSum(const TLorentzVector & nup4)
{
// Computes the sum of the cross sections for all the interactions that can
// be simulated for the given initial state and for the input neutrino energy
//
  LOG("GEVGDriver", pDEBUG) << "Computing the cross section sum";

  double xsec_sum = 0;

  // Get the list of spline objects
  // Should have been constructed at the job initialization
  XSecSplineList * xssl = XSecSplineList::Instance();

  // Get the list of all interactions that can be generated by this driver
  const InteractionList & ilst = fXSecAlgorithmMap->GetInteractionList();

  // Loop over all interactions & compute cross sections
  InteractionList::const_iterator intliter;
  for(intliter = ilst.begin(); intliter != ilst.end(); ++intliter) {

     // get current interaction
     Interaction * interaction = new Interaction(**intliter);
     interaction->GetInitialStatePtr()->SetProbeP4(nup4);

     string code = interaction->AsString();
     SLOG("GEVGDriver", pDEBUG)
             << "Compute cross section for interaction: \n" << code;

     // get corresponding cross section algorithm
     const XSecAlgorithmI * xsec_alg =
                  fXSecAlgorithmMap->FindXSecAlgorithm(interaction);
     assert(xsec_alg);

     // compute (or evaluate) the cross section
     double xsec = 0;
     bool spline_exists = xssl->SplineExists(xsec_alg, interaction);
     if (spline_exists && fUseSplines) {
        double E = nup4.Energy();
        xsec = xssl->GetSpline(xsec_alg,interaction)->Evaluate(E);
     } else
        xsec = xsec_alg->XSec(interaction);

     // sum-up and report
     xsec_sum += xsec;
     LOG("GEVGDriver", pDEBUG)
            << "\nInteraction   = " << code
            << "\nCross Section "
            << (fUseSplines ? "*interpolated*" : "*computed*")
            << " = " << (xsec/units::cm2) << " cm2";

     delete interaction;
  } // loop over event generators

  PDGLibrary * pdglib = PDGLibrary::Instance();
  LOG("GEVGDriver", pINFO)
    << "SumXSec("
    << pdglib->Find(fInitState->GetProbePDGCode())->GetName() << "+"
    << pdglib->Find(fInitState->GetTarget().PDGCode())->GetName() << "->X, "
    << "E = " << nup4.Energy() << " GeV)"
    << (fUseSplines ? "*interpolated*" : "*computed*")
    << " = " << (xsec_sum/units::cm2) << " cm2";

  return xsec_sum;
}
//___________________________________________________________________________
void GEVGDriver::CreateXSecSumSpline(
                               int nk, double Emin, double Emax, bool inlogE)
{
// This method creates a spline with the *total* cross section vs E (or logE)
// for the initial state that this driver was configured with.
// This spline is used, for example, by the GMCJDriver to select a target
// material out of all the materials in a detector geometry (summing the
// cross sections again and again proved to be expensive...)

  LOG("GEVGDriver", pNOTICE)
     << "Creating spline (sum-xsec = f(" << ((inlogE) ? "logE" : "E")
     << ") in E = [" << Emin << ", " << Emax << "] using " << nk << " knots";

  if(!fUseSplines) {
     LOG("GEVGDriver", pFATAL) << "You haven't loaded any splines!! ";
  }
  assert(fUseSplines);
  assert(Emin<Emax && Emin>0 && nk>2);

  double logEmin=0, logEmax=0, dE=0;

  double * E    = new double[nk];
  double * xsec = new double[nk];

  if(inlogE) {
    logEmin = TMath::Log(Emin);
    logEmax = TMath::Log(Emax);
    dE = (logEmax-logEmin)/(nk-1);
  } else {
    dE = (Emax-Emin)/(nk-1);
  }

  TLorentzVector p4(0,0,0,0);

  for(int i=0; i<nk; i++) {

    double e = (inlogE) ? TMath::Exp(logEmin + i*dE) : Emin + i*dE;

    p4.SetPxPyPzE(0.,0.,e,e);
    double xs = this->XSecSum(p4);

    E[i]    = e;
    xsec[i] = xs;
  }
  if (fXSecSumSpl) delete fXSecSumSpl;
  fXSecSumSpl = new Spline(nk, E, xsec);
}
//___________________________________________________________________________
void GEVGDriver::UseSplines(void)
{
// Instructs the driver to use cross section splines rather than computing
// them again and again.
// **Note**
// -- If you called GEVGDriver::CreateSplines() already the driver would
//    a) assume that you want to use them and b) would know that it has all
//    the splines it needs, so you do not need to call this method.
// -- If you populated the XSecSplineList in another way, eg from an external
//    XML file, this driver has no way to know. So do call this method then.
//    However, the driver would **explicitly check** that you loaded all the
//    splines it needs. If not, then its fiery personality will take over and
//    it will refuse your request, reverting back to not using splines.

  fUseSplines = true;

  // Get the list of spline objects
  // Should have been constructed at the job initialization
  XSecSplineList * xsl = XSecSplineList::Instance();

  // If the user wants to use splines, make sure that all the splines needed
  // have been computed or loaded
  if(fUseSplines) {

    // Get the list of all interactions that can be generated by this driver
    const InteractionList & ilst = fXSecAlgorithmMap->GetInteractionList();

    // Loop over all interactions & check that all splines have been loaded
    InteractionList::const_iterator intliter;
    for(intliter = ilst.begin(); intliter != ilst.end(); ++intliter) {

       // current interaction
       Interaction * interaction = *intliter;

       // corresponding cross section algorithm
       const XSecAlgorithmI * xsec_alg =
                   fXSecAlgorithmMap->FindXSecAlgorithm(interaction);
       assert(xsec_alg);

       // spline exists in spline list?
       bool spl_exists = xsl->SplineExists(xsec_alg, interaction);

       // update the 'use splines' flag
       fUseSplines = fUseSplines && spl_exists;

       if(!spl_exists) {
          LOG("GEVGDriver", pWARN)
              << "\nAt least a spline does not exist. "
                               << "Reverting back to not using splines";
          return;
       }
     } // loop over interaction list
  }//use-splines?
}
//___________________________________________________________________________
void GEVGDriver::CreateSplines(bool useLogE)
{
// Creates all the cross section splines that are needed by this driver.
// It will check for pre-loaded splines and it will skip the creation of the
// splines it already finds loaded.

  LOG("GEVGDriver", pINFO)
       << "\nCreating (missing) xsec splines with UseLogE = "
                                               << ((useLogE) ? "ON" : "OFF");
  // Get the list of spline objects
  XSecSplineList * xsl = XSecSplineList::Instance();
  xsl->SetLogE(useLogE);

  EventGeneratorList::const_iterator evgliter; // event generator list iter
  InteractionList::iterator          intliter; // interaction list iter

  // loop over all EventGenerator objects used in the current job
  for(evgliter = fEvGenList->begin();
                               evgliter != fEvGenList->end(); ++evgliter) {
     // current event generator
     const EventGeneratorI * evgen = *evgliter;
     LOG("GEVGDriver", pNOTICE)
            << "Querying [ " << evgen->Id().Key()
                                            << "] for its InteractionList";

     // ask the event generator to produce a list of all interaction it can
     // generate for the input initial state
     const InteractionListGeneratorI * ilstgen = evgen->IntListGenerator();
     InteractionList * ilst = ilstgen->CreateInteractionList(*fInitState);
     if(!ilst) continue;

     // total cross section algorithm used by the current EventGenerator
     const XSecAlgorithmI * alg = evgen->CrossSectionAlg();

     // get the energy range of the spline from the EventGenerator
     // validity context
     double Emin = TMath::Max(0.01,evgen->ValidityContext().Emin());
     double Emax = evgen->ValidityContext().Emax();

     // loop over all interaction that can be genererated and ask the
     // appropriate cross section algorithm to compute its cross section
     for(intliter = ilst->begin(); intliter != ilst->end(); ++intliter) {

         // current interaction
         Interaction * interaction = *intliter;

         // create a cross section spline for this interaction & store
         LOG("GEVGDriver", pNOTICE)
               << "\nNeed xsec spline for " << interaction->AsString();

         // only create the spline if it does not already exists
         bool spl_exists = xsl->SplineExists(alg, interaction);
         if(!spl_exists) {
             LOG("GEVGDriver", pINFO) << "Computing spline knots";
             xsl->CreateSpline(alg, interaction, 40, Emin, Emax);
         } else {
             LOG("GEVGDriver", pNOTICE)
                         << "Spline is already loaded - skipping";
         }
     } // loop over interaction that can be generated by this generator
     delete ilst;
     ilst = 0;
  } // loop over event generators

  LOG("GEVGDriver", pINFO) << *xsl; // print list of splines

  fUseSplines = true;
}
//___________________________________________________________________________
Range1D_t GEVGDriver::ValidEnergyRange(void) const
{
// loops over all loaded event generation threads, queries for the energy
// range at their validity context and builds the valid energy range for
// this driver

  Range1D_t E;
  E.min =  9999;
  E.max = -9999;

  EventGeneratorList::const_iterator evgliter; // event generator list iter
  InteractionList::iterator          intliter; // interaction list iter

  // loop over all EventGenerator objects used in the current job
  for(evgliter = fEvGenList->begin();
                               evgliter != fEvGenList->end(); ++evgliter) {
     // current event generator
     const EventGeneratorI * evgen = *evgliter;

     // Emin, Emax as declared in current generator's validity context
     double Emin = TMath::Max(0.01,evgen->ValidityContext().Emin());
     double Emax = evgen->ValidityContext().Emax();

     // combined Emin, Emax
     E.min = TMath::Min(E.min, Emin);
     E.max = TMath::Max(E.max, Emax);
  }

  assert(E.min<E.max && E.min>=0);
  return E;
}
//___________________________________________________________________________
void GEVGDriver::AssertIsValidInitState(void) const
{
  assert(fInitState);

  int nu_pdgc = fInitState->GetProbePDGCode();

  bool isnu = pdg::IsNeutrino(nu_pdgc) || pdg::IsAntiNeutrino(nu_pdgc);
  assert(isnu);
}
//___________________________________________________________________________
void GEVGDriver::Print(ostream & stream) const
{
  stream
    << "\n\n *********************** GEVGDriver ***************************";

  int nupdg  = fInitState->GetProbePDGCode();
  int tgtpdg = fInitState->GetTarget().PDGCode();

  stream << "\n  |---o Neutrino PDG-code .........: " << nupdg;
  stream << "\n  |---o Nuclear Target PDG-code ...: " << tgtpdg;

  stream << "\n  |---o Using cross section splines is turned "
                                << utils::print::BoolAsIOString(fUseSplines);
  stream << "\n  |---o Filtering unphysical events is turned "
                          << utils::print::BoolAsIOString(fFilterUnphysical);

  stream << "\n *********************************************************\n";
}
//___________________________________________________________________________

