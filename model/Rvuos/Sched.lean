/-
The real-time scheduler model for rvuos: virtual time deadlines,
sub-tick crossing timer with hysteresis, and delay guarantees.
See decision 39 in design/decisions.md and the busy-period derivation in design/time.md, "Scheduling".

The core has a total capacity of U = 64 units (TIME_UNITS).
A target place T is queued at count t0 with units uT (1 ≤ uT ≤ 64),
deficit defT, and delay window wT.
Its virtual deadline at arrival is dT = t0 + defT + wT.
Other places on the core hold at most 64 - uT units in total.

This module proves:
1. The Bandwidth Lemma: total competitor work before dT is bounded by
   (64 - uT) * (dT - t0) / 64.
2. The Initial Potential Bound: initState.potential ≤ maxWait.
3. Inductive Invariant Preservation: s.waited + s.potential ≤ maxWait
   is preserved by every operational step of the scheduler (kernel blocking,
   EDF execution, hysteresis crossing, sleeping, anchor-rule waking, rotation).
4. The Start Bound (both arrival at t0 and general busy period starting at tau ≤ t0):
   T acquires the processor within maxWait counts of t0.
5. Non-vacuity: a mutant scheduler that selects a competitor with deadline > dT
   strictly violates the delay invariant.
-/

namespace Rvuos

/-- Total units per core. -/
def timeUnits : Nat := 64

/-- The Start Bound: maximum counts place T waits before acquiring the processor.
    `qmin` is the kernel's crossing quantum, `CROSS_QUANTUM`: a per-board count of the counter
    that covers one turn switch and the kernel's way back to user mode from the comparator write,
    so a crossing costs a switch, not a count. The proof needs only `qmin ≥ 1`. -/
def maxWait (uT defT wT N qmin B : Nat) : Nat :=
  B + N * qmin + ((timeUnits - uT) * (defT + wT) + (timeUnits - 1)) / timeUnits

/-- A competitor place on the core.
    Virtual time v represents u * d (virtual deadline scaled by u). -/
structure Competitor where
  id : Nat
  u : Nat
  v : Nat
  active : Bool
  /- The counts this competitor may still run past the crossing before it is preempted,
     at most the kernel's `CROSS_QUANTUM`; its turn costs a switch, not a bare count. -/
  qmin_rem : Nat
  crossed : Bool
deriving Repr, Inhabited

/-- Work competitor c can execute before its virtual deadline exceeds dT. -/
def compWork (c : Competitor) (dT : Nat) : Nat :=
  if c.v ≤ c.u * dT then (c.u * dT - c.v) / timeUnits else 0

/-- Potential of competitor c: work before dT plus remaining hysteresis allowance. -/
def compPotential (c : Competitor) (dT : Nat) : Nat :=
  compWork c dT + if c.crossed then 0 else c.qmin_rem

/-- Sum of shares of a list of competitors. -/
def sumU : List Competitor → Nat
  | [] => 0
  | c :: cs => c.u + sumU cs

/-- Sum of virtual distance to dT. -/
def sumD (cs : List Competitor) (dT : Nat) : Nat :=
  match cs with
  | [] => 0
  | c :: rest => (if c.v ≤ c.u * dT then c.u * dT - c.v else 0) + sumD rest dT

/-- Sum of work before dT across competitors. -/
def sumWork (cs : List Competitor) (dT : Nat) : Nat :=
  match cs with
  | [] => 0
  | c :: rest => compWork c dT + sumWork rest dT

/-- Sum of hysteresis allowances. -/
def sumQmin : List Competitor → Nat
  | [] => 0
  | c :: rest => (if c.crossed then 0 else c.qmin_rem) + sumQmin rest

/-- Total potential of a list of competitors. -/
def listPotential : List Competitor → Nat → Nat
  | [], _ => 0
  | c :: cs, dT => compPotential c dT + listPotential cs dT

@[simp] theorem listPotential_nil (dT : Nat) : listPotential [] dT = 0 := rfl
@[simp] theorem listPotential_cons (c : Competitor) (cs : List Competitor) (dT : Nat) :
    listPotential (c :: cs) dT = compPotential c dT + listPotential cs dT := rfl

theorem listPotential_eq_sum (cs : List Competitor) (dT : Nat) :
    listPotential cs dT = sumWork cs dT + sumQmin cs := by
  induction cs with
  | nil => simp [listPotential, sumWork, sumQmin]
  | cons c cs ih =>
    simp only [listPotential_cons, sumWork, sumQmin, compPotential]
    omega

/-! ## Helper lemmas on competitor lists -/

theorem sumWork_le_sumD (cs : List Competitor) (dT : Nat) :
    sumWork cs dT ≤ sumD cs dT / timeUnits := by
  induction cs with
  | nil => simp [sumWork, sumD, timeUnits]
  | cons c cs ih =>
    simp only [sumWork, sumD]
    have hdiv : compWork c dT + sumD cs dT / timeUnits ≤
        ((if c.v ≤ c.u * dT then c.u * dT - c.v else 0) + sumD cs dT) / timeUnits := by
      unfold compWork timeUnits
      split <;> omega
    omega

theorem sumD_le_sumU_mul (cs : List Competitor) (t0 dT : Nat)
    (hinit : ∀ c ∈ cs, c.v ≤ c.u * dT → c.v ≥ c.u * t0) :
    sumD cs dT ≤ sumU cs * (dT - t0) := by
  induction cs with
  | nil => simp [sumD, sumU]
  | cons c cs ih =>
    simp only [sumD, sumU]
    have hc : (if c.v ≤ c.u * dT then c.u * dT - c.v else 0) ≤ c.u * (dT - t0) := by
      split
      · rename_i hle
        have hge := hinit c (by simp) hle
        have hmul : c.u * dT - c.v ≤ c.u * dT - c.u * t0 := by omega
        have hdistrib : c.u * (dT - t0) = c.u * dT - c.u * t0 := Nat.mul_sub_left_distrib c.u dT t0
        omega
      · omega
    have hcs : ∀ c' ∈ cs, c'.v ≤ c'.u * dT → c'.v ≥ c'.u * t0 := fun c' hc' => hinit c' (by simp [hc'])
    have ih' := ih hcs
    have hdistrib : (c.u + sumU cs) * (dT - t0) = c.u * (dT - t0) + sumU cs * (dT - t0) :=
      Nat.add_mul c.u (sumU cs) (dT - t0)
    omega

/-- The Bandwidth Lemma: competitor work with deadline ≤ dT is bounded by
    the competitor share of the busy period. -/
theorem bandwidth_lemma (cs : List Competitor) (t0 dT uT : Nat)
    (hsumU : sumU cs ≤ timeUnits - uT)
    (hinit : ∀ c ∈ cs, c.v ≤ c.u * dT → c.v ≥ c.u * t0) :
    sumWork cs dT ≤ ((timeUnits - uT) * (dT - t0)) / timeUnits := by
  have h1 := sumWork_le_sumD cs dT
  have h2 := sumD_le_sumU_mul cs t0 dT hinit
  have hmul : sumU cs * (dT - t0) ≤ (timeUnits - uT) * (dT - t0) :=
    Nat.mul_le_mul_right (dT - t0) hsumU
  have hdiv : sumD cs dT / timeUnits ≤ ((timeUnits - uT) * (dT - t0)) / timeUnits := by
    unfold timeUnits at *
    omega
  omega

theorem sumQmin_le (cs : List Competitor) (qmin : Nat)
    (hq : ∀ c ∈ cs, c.qmin_rem ≤ qmin) :
    sumQmin cs ≤ cs.length * qmin := by
  induction cs with
  | nil => simp [sumQmin]
  | cons c cs ih =>
    simp only [sumQmin, List.length_cons]
    have hc : (if c.crossed then 0 else c.qmin_rem) ≤ qmin := by
      split
      · omega
      · exact hq c (by simp)
    have hcs : ∀ c' ∈ cs, c'.qmin_rem ≤ qmin := fun c' hc' => hq c' (by simp [hc'])
    have ih' := ih hcs
    rw [Nat.succ_mul]
    omega

theorem listPotential_append (xs ys : List Competitor) (dT : Nat) :
    listPotential (xs ++ ys) dT = listPotential xs dT + listPotential ys dT := by
  induction xs with
  | nil => simp
  | cons x xs ih =>
    simp only [List.cons_append, listPotential_cons, ih]
    omega

theorem listPotential_rotate (c : Competitor) (cs : List Competitor) (dT : Nat) :
    listPotential (cs ++ [c]) dT = listPotential (c :: cs) dT := by
  rw [listPotential_append, listPotential_cons, listPotential_nil]
  dsimp
  omega

theorem work_step_exact (M v : Nat) (h : v + 64 ≤ M) :
    (M - (v + 64)) / 64 + 1 = (M - v) / 64 := by
  omega

theorem compwork_zero (u dT v : Nat) (h : u * dT < v + 64) :
    (if v ≤ u * dT then (u * dT - v) / 64 else 0) = 0 := by
  split <;> omega

theorem compwork_after_zero (u dT v : Nat) (h : u * dT < v + 64) :
    (if v + 64 ≤ u * dT then (u * dT - (v + 64)) / 64 else 0) = 0 := by
  split <;> omega

theorem compPotential_wake (c : Competitor) (v' : Nat) (hv : v' ≥ c.v) (dT : Nat) :
    compPotential { c with active := true, v := v' } dT ≤ compPotential c dT := by
  have hw : compWork { c with active := true, v := v' } dT ≤ compWork c dT := by
    unfold compWork timeUnits
    dsimp
    by_cases h1 : v' ≤ c.u * dT
    · have h2 : c.v ≤ c.u * dT := by omega
      simp only [h1, h2, ↓reduceIte]
      omega
    · simp only [h1, ↓reduceIte]
      split <;> omega
  unfold compPotential
  dsimp
  omega

theorem compPotential_cross (c : Competitor) (dT : Nat) :
    compPotential { c with crossed := true } dT ≤ compPotential c dT := by
  unfold compPotential compWork timeUnits
  dsimp
  split <;> omega

theorem tau_delta_le (X uT delta : Nat) (hu64 : uT ≤ timeUnits) :
    (X + (timeUnits - uT) * delta) / timeUnits ≤ X / timeUnits + delta := by
  have hu : uT ≤ 64 := hu64
  have hmul : (64 - uT) * delta ≤ 64 * delta := Nat.mul_le_mul_right delta (by omega)
  have hadd : X + (64 - uT) * delta ≤ X + 64 * delta := by omega
  have hdiv1 : (X + (64 - uT) * delta) / 64 ≤ (X + 64 * delta) / 64 := by omega
  have hdiv2 : (X + 64 * delta) / 64 = X / 64 + delta := by omega
  show (X + (64 - uT) * delta) / 64 ≤ X / 64 + delta
  omega

theorem tau_potential_le (X uT delta : Nat) (hu64 : uT ≤ timeUnits) :
    (X + (timeUnits - uT) * delta) / timeUnits - delta ≤ X / timeUnits := by
  have h := tau_delta_le X uT delta hu64
  show (X + (64 - uT) * delta) / 64 - delta ≤ X / 64
  have h' : (X + (64 - uT) * delta) / 64 ≤ X / 64 + delta := h
  omega

theorem maxWait_tau_le (uT defT wT N qmin B delta : Nat) (hu64 : uT ≤ timeUnits) :
    maxWait uT (defT + delta) wT N qmin B - delta ≤ maxWait uT defT wT N qmin B := by
  unfold maxWait
  have hdistrib : (timeUnits - uT) * (defT + delta + wT) =
      (timeUnits - uT) * (defT + wT) + (timeUnits - uT) * delta := by
    have : defT + delta + wT = (defT + wT) + delta := by omega
    rw [this]
    exact Nat.mul_add (timeUnits - uT) (defT + wT) delta
  rw [hdistrib]
  have hassoc : (timeUnits - uT) * (defT + wT) + (timeUnits - uT) * delta + (timeUnits - 1) =
      ((timeUnits - uT) * (defT + wT) + (timeUnits - 1)) + (timeUnits - uT) * delta := by omega
  rw [hassoc]
  have htau := tau_potential_le ((timeUnits - uT) * (defT + wT) + (timeUnits - 1)) uT delta hu64
  omega

/-! ## Discrete operational state of the scheduler -/

structure SchedState where
  waited : Nat
  block_rem : Nat
  uT : Nat
  t0 : Nat
  defT : Nat
  wT : Nat
  competitors : List Competitor
deriving Repr, Inhabited

def SchedState.dT (s : SchedState) : Nat :=
  s.t0 + s.defT + s.wT

def SchedState.potential (s : SchedState) : Nat :=
  s.block_rem + listPotential s.competitors s.dT

def SchedState.invariant (s : SchedState) (bound : Nat) : Prop :=
  s.waited + s.potential ≤ bound

def initState (uT t0 defT wT B : Nat) (comp : List Competitor) : SchedState :=
  ⟨0, B, uT, t0, defT, wT, comp⟩

/-- The initial potential satisfies the bound maxWait. -/
theorem init_potential_bound (uT t0 defT wT B qmin : Nat) (comp : List Competitor)
    (hsumU : sumU comp ≤ timeUnits - uT)
    (hinit : ∀ c ∈ comp, c.v ≤ c.u * (t0 + defT + wT) → c.v ≥ c.u * t0)
    (hq : ∀ c ∈ comp, c.qmin_rem ≤ qmin) :
    (initState uT t0 defT wT B comp).potential ≤ maxWait uT defT wT comp.length qmin B := by
  unfold SchedState.potential initState SchedState.dT
  dsimp only
  rw [listPotential_eq_sum]
  have hbw := bandwidth_lemma comp t0 (t0 + defT + wT) uT hsumU hinit
  have hqle := sumQmin_le comp qmin hq
  have hsub : (t0 + defT + wT) - t0 = defT + wT := by omega
  rw [hsub] at hbw
  unfold maxWait timeUnits at *
  omega

/-! ## Operational transitions of the scheduler -/

inductive Step : SchedState → SchedState → Prop
  /-- 1. Initial kernel non-preemptible step (B). -/
  | kernel_step (s : SchedState) (hb : s.block_rem > 0) :
      Step s { s with waited := s.waited + 1, block_rem := s.block_rem - 1 }
  /-- 2. Competitor executes 1 count of work under EDF with deadline ≤ dT. -/
  | comp_work (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor)
      (hblock : b = 0) (hact : c.active = true) (hwork : c.v + 64 ≤ c.u * (t0 + defT + wT)) :
      Step ⟨w, b, uT, t0, defT, wT, c :: cs⟩
           ⟨w + 1, b, uT, t0, defT, wT, { c with v := c.v + 64 } :: cs⟩
  /-- 3. Competitor executes 1 count on hysteresis after reaching or crossing dT. -/
  | comp_hysteresis (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor)
      (hblock : b = 0) (hact : c.active = true) (hcross : c.crossed = false)
      (hq : c.qmin_rem > 0) (hover : c.u * (t0 + defT + wT) < c.v + 64) :
      Step ⟨w, b, uT, t0, defT, wT, c :: cs⟩
           ⟨w + 1, b, uT, t0, defT, wT, { c with v := c.v + 64, qmin_rem := c.qmin_rem - 1 } :: cs⟩
  /-- 4. Competitor completes its hysteresis turn. -/
  | comp_cross (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor) :
      Step ⟨w, b, uT, t0, defT, wT, c :: cs⟩
           ⟨w, b, uT, t0, defT, wT, { c with crossed := true } :: cs⟩
  /-- 5. Competitor yields or sleeps. -/
  | comp_sleep (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor) :
      Step ⟨w, b, uT, t0, defT, wT, c :: cs⟩
           ⟨w, b, uT, t0, defT, wT, { c with active := false } :: cs⟩
  /-- 6. Competitor wakes up at current time t with anchor rule v' = max(v, u * t) ≥ v. -/
  | comp_wake (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor)
      (v' : Nat) (hv : v' ≥ c.v) :
      Step ⟨w, b, uT, t0, defT, wT, c :: cs⟩
           ⟨w, b, uT, t0, defT, wT, { c with active := true, v := v' } :: cs⟩
  /-- 7. Queue rotation. -/
  | rotate (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor) :
      Step ⟨w, b, uT, t0, defT, wT, c :: cs⟩
           ⟨w, b, uT, t0, defT, wT, cs ++ [c]⟩
  /-- 8. Target place T acquires the processor (terminal state for waiting). -/
  | target_acquire (s : SchedState) (hblock : s.block_rem = 0) :
      Step s s

/-! ## Inductive preservation of the invariant -/

theorem step_preserves_invariant {s s' : SchedState} {bound : Nat}
    (hstep : Step s s') (hinv : s.invariant bound) : s'.invariant bound := by
  cases hstep with
  | kernel_step s hb =>
    unfold SchedState.invariant SchedState.potential SchedState.dT at *
    dsimp at *
    omega
  | comp_work w b uT t0 defT wT c cs hblock hact hwork =>
    unfold SchedState.invariant SchedState.potential SchedState.dT listPotential compPotential compWork at *
    dsimp at *
    unfold timeUnits at *
    have hv : c.v ≤ c.u * (t0 + defT + wT) := by omega
    have hstep := work_step_exact (c.u * (t0 + defT + wT)) c.v hwork
    simp only [hv, hwork, ↓reduceIte] at *
    omega
  | comp_hysteresis w b uT t0 defT wT c cs hblock hact hcross hq hover =>
    unfold SchedState.invariant SchedState.potential SchedState.dT listPotential compPotential compWork at *
    dsimp at *
    unfold timeUnits at *
    have hz1 := compwork_zero c.u (t0 + defT + wT) c.v hover
    have hz2 := compwork_after_zero c.u (t0 + defT + wT) c.v hover
    have hnot : ¬ (c.v + 64 ≤ c.u * (t0 + defT + wT)) := by omega
    simp only [hcross, Bool.false_eq_true, ↓reduceIte, hz1, hz2] at *
    omega
  | comp_cross w b uT t0 defT wT c cs =>
    unfold SchedState.invariant SchedState.potential SchedState.dT listPotential at *
    dsimp at *
    have hpot := compPotential_cross c (t0 + defT + wT)
    omega
  | comp_sleep w b uT t0 defT wT c cs =>
    unfold SchedState.invariant SchedState.potential SchedState.dT listPotential compPotential compWork at *
    exact hinv
  | comp_wake w b uT t0 defT wT c cs v' hv =>
    unfold SchedState.invariant SchedState.potential SchedState.dT listPotential at *
    dsimp at *
    have hpot := compPotential_wake c v' hv (t0 + defT + wT)
    omega
  | rotate w b uT t0 defT wT c cs =>
    unfold SchedState.invariant SchedState.potential SchedState.dT at *
    dsimp at *
    rw [listPotential_rotate]
    exact hinv
  | target_acquire s hblock =>
    exact hinv

inductive Steps : SchedState → SchedState → Prop
  | refl (s : SchedState) : Steps s s
  | tail {s1 s2 s3 : SchedState} : Steps s1 s2 → Step s2 s3 → Steps s1 s3

theorem steps_preserves_invariant {s s' : SchedState} {bound : Nat}
    (hsteps : Steps s s') (hinv : s.invariant bound) : s'.invariant bound := by
  induction hsteps with
  | refl => exact hinv
  | tail _ h23 ih =>
    exact step_preserves_invariant h23 ih

/-! ## The Start Bound Theorems -/

/-- The Start Bound: for any state reached by the scheduler from a valid initial state at arrival t0,
    the time place T waited before acquiring the core is bounded by maxWait. -/
theorem start_bound {uT t0 defT wT B qmin : Nat} {comp : List Competitor} {s : SchedState}
    (_hu : uT ≤ timeUnits)
    (_hqmin : qmin ≥ 1)
    (hsumU : sumU comp ≤ timeUnits - uT)
    (hinit : ∀ c ∈ comp, c.v ≤ c.u * (t0 + defT + wT) → c.v ≥ c.u * t0)
    (hq : ∀ c ∈ comp, c.qmin_rem ≤ qmin)
    (hsteps : Steps (initState uT t0 defT wT B comp) s) :
    s.waited ≤ maxWait uT defT wT comp.length qmin B := by
  have hbound := init_potential_bound uT t0 defT wT B qmin comp hsumU hinit hq
  have hinit_inv : (initState uT t0 defT wT B comp).invariant (maxWait uT defT wT comp.length qmin B) := by
    unfold SchedState.invariant initState
    dsimp only [SchedState.waited]
    rw [Nat.zero_add]
    exact hbound
  have hinv := steps_preserves_invariant hsteps hinit_inv
  unfold SchedState.invariant SchedState.potential at hinv
  omega

/-- The Start Bound with Busy Period starting at tau ≤ t0:
    If the busy period starts at tau with delta = t0 - tau counts of work executed
    by level ≤ dT competitors before T arrives at t0, reaching state st0 with st0.waited = delta,
    then the time T waits from its arrival at t0 is bounded by maxWait. -/
theorem start_bound_busy_period {uT tau t0 defT wT B qmin : Nat} {comp : List Competitor}
    {st0 s : SchedState}
    (_htau : tau ≤ t0)
    (hu : uT ≤ timeUnits)
    (_hqmin : qmin ≥ 1)
    (hsumU : sumU comp ≤ timeUnits - uT)
    (hinit : ∀ c ∈ comp, c.v ≤ c.u * (tau + (defT + (t0 - tau)) + wT) → c.v ≥ c.u * tau)
    (hq : ∀ c ∈ comp, c.qmin_rem ≤ qmin)
    (hdelta : Steps (initState uT tau (defT + (t0 - tau)) wT B comp) st0)
    (_hwaiting : st0.waited = t0 - tau)
    (hsteps : Steps st0 s) :
    s.waited - st0.waited ≤ maxWait uT defT wT comp.length qmin B := by
  let delta := t0 - tau
  have hbound_tau := init_potential_bound uT tau (defT + delta) wT B qmin comp hsumU hinit hq
  have hinit_inv : (initState uT tau (defT + delta) wT B comp).invariant
      (maxWait uT (defT + delta) wT comp.length qmin B) := by
    unfold SchedState.invariant initState
    dsimp only [SchedState.waited]
    rw [Nat.zero_add]
    exact hbound_tau
  have hinv_st0 := steps_preserves_invariant hdelta hinit_inv
  have hsteps_all : Steps (initState uT tau (defT + delta) wT B comp) s := by
    induction hsteps with
    | refl => exact hdelta
    | tail _ step ih => exact Steps.tail ih step
  have hinv_s := steps_preserves_invariant hsteps_all hinit_inv
  unfold SchedState.invariant at hinv_st0 hinv_s
  have hle := maxWait_tau_le uT defT wT comp.length qmin B delta hu
  omega

/-! ## Non-vacuity: Mutant Counterexample -/

/-- A mutated scheduler step (like earliest-picks-latest) that selects a competitor
    whose deadline has already passed dT and whose hysteresis is exhausted. -/
inductive MutantStep : SchedState → SchedState → Prop
  | bad_pick (w b uT t0 defT wT : Nat) (c : Competitor) (cs : List Competitor)
      (hdead : c.v ≥ c.u * (t0 + defT + wT)) (hcross : c.crossed = true) :
      MutantStep ⟨w, b, uT, t0, defT, wT, c :: cs⟩
                 ⟨w + 1, b, uT, t0, defT, wT, { c with v := c.v + 64 } :: cs⟩

/-- The mutant step breaks the delay invariant: selecting a competitor with deadline > dT
    allows the waiting time to exceed the bound. -/
theorem mutant_breaks_invariant :
    ∃ (s s' : SchedState) (bound : Nat),
      s.invariant bound ∧
      MutantStep s s' ∧
      ¬ s'.invariant bound := by
  let c : Competitor := ⟨1, 1, 100, true, 0, true⟩
  let s : SchedState := ⟨10, 0, 7, 0, 100, 0, [c]⟩
  let s' : SchedState := ⟨11, 0, 7, 0, 100, 0, [{ c with v := 164 }]⟩
  have hstep : MutantStep s s' := by
    apply MutantStep.bad_pick
    · simp [c]
    · rfl
  have hinv : s.invariant 10 := by
    simp [SchedState.invariant, SchedState.potential, SchedState.dT, compPotential, compWork, s, c]
  have hnot : ¬ s'.invariant 10 := by
    simp [SchedState.invariant, SchedState.potential, SchedState.dT, compPotential, compWork, s', c]
  exact ⟨s, s', 10, hinv, hstep, hnot⟩

end Rvuos
