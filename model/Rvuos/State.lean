/-
The abstract state: what `host/model.c` writes of the kernel's memory.
Objects, the filled nodes of the derivation tree by their addresses,
and each node's children in the order of their ring.
Links are left out: the tree is the children lists, and a node's parent is the one whose list holds it.
-/
import Rvuos.Abi

namespace Rvuos

/-- A node's words: a filled `struct cap` without its links. -/
structure Cap where
  type : Nat
  rights : Nat
  index : Nat
  a : Nat
  b : Nat
deriving BEq, Repr, Inhabited, DecidableEq

/-- An Untyped's `b` once its delete as a root has begun, after which it makes nothing:
the kernel's `B_GOING`, as `host/model.c` writes it. -/
abbrev untypedGoing : Nat := 1

/-- A node a delete of it as a root has begun: an Untyped marked going, anything else as it was. -/
def Cap.markGoing (c : Cap) : Cap := if c.type = capUntyped then { c with b := untypedGoing } else c

/-- An object: its address, type and pool, the array of nodes it holds, from `obj_nodes`, and a pool its size. -/
structure Obj where
  addr : Nat
  type : Nat
  pool : Nat
  nodes : Nat
  count : Nat
  /-- A pool's size, which its own node stands for; 0 for any other object. -/
  size : Nat
deriving BEq, Repr, Inhabited, DecidableEq

structure State where
  objs : List Obj
  caps : List (Nat × Cap)
  kids : List (Nat × List Nat)
deriving BEq, Repr, Inhabited

/-- What the machine fixes, which `host/model.c` writes with every step and where the records begin. -/
structure Machine where
  /-- `region_min_size()`, the smallest block a frame may be. -/
  regionMin : Nat
  /-- `POOL_MIN_SIZE`. -/
  poolMin : Nat
  /-- Where in a process its table slots lie, in a table its slots, in a thread its process. -/
  procTablesOff : Nat
  tableSlotsOff : Nat
  threadProcOff : Nat
deriving Repr, Inhabited

/-- The value at a key of an association list, the first if there are more. -/
def assoc {α : Type} : List (Nat × α) → Nat → Option α
  | [], _ => none
  | (k, v) :: l, n => if k = n then some v else assoc l n

/-- An association list without a key. -/
def unkey {α : Type} (l : List (Nat × α)) (n : Nat) : List (Nat × α) := l.filter (fun p => decide (p.1 ≠ n))

namespace State

def obj? (s : State) (a : Nat) : Option Obj := s.objs.find? (·.addr == a)

def cap? (s : State) (n : Nat) : Option Cap := assoc s.caps n

def kidsOf (s : State) (n : Nat) : List Nat := (assoc s.kids n).getD []

def parent? (s : State) (n : Nat) : Option Nat := (s.kids.find? (fun p => p.2.contains n)).map (·.1)

def setCap (s : State) (n : Nat) (c : Cap) : State :=
  { s with caps := (n, c) :: unkey s.caps n }

def eraseCap (s : State) (n : Nat) : State :=
  { s with caps := unkey s.caps n }

/-- A node's children, none at all leaving no entry. -/
def setKids (s : State) (n : Nat) (l : List Nat) : State :=
  let rest := unkey s.kids n
  { s with kids := if l.isEmpty then rest else (n, l) :: rest }

/-- `cap_attach`: at the front of the parent's ring, or a root without a parent. -/
def attach (s : State) (parent : Option Nat) (n : Nat) : State :=
  match parent with
  | none => s
  | some p => s.setKids p (n :: s.kidsOf p)

/-- Right after `x` in a list, or the list as it was where `x` is not in it. -/
def insertAfter (x n : Nat) : List Nat → List Nat
  | [] => []
  | y :: ys => if y == x then y :: n :: ys else y :: insertAfter x n ys

/-- `attach_beside`: right after a node in its parent's ring; beside a root, a root. -/
def attachBeside (s : State) (beside n : Nat) : State :=
  match s.parent? beside with
  | none => s
  | some p => s.setKids p (insertAfter beside n (s.kidsOf p))

/-- Each `x` in a list in place of the list `r`. -/
def replaceWith (x : Nat) (r : List Nat) : List Nat → List Nat
  | [] => []
  | y :: ys => if y == x then r ++ replaceWith x r ys else y :: replaceWith x r ys

/-- `cap_move`: the node at `to`, where `from` stood, its words and children with it. -/
def move (s : State) (src dst : Nat) : State :=
  match s.cap? src with
  | none => s
  | some c =>
    let s1 := (s.eraseCap src).setCap dst c
    let s2 := match s.parent? src with
      | none => s1
      | some p => s1.setKids p (replaceWith src [dst] (s.kidsOf p))
    (s2.setKids src []).setKids dst (s.kidsOf src)

/-- `detach` with adopt: a node that has a parent leaves its ring, its children taking its place. -/
def detachAdopt (s : State) (n : Nat) : State :=
  match s.parent? n with
  | none => s
  | some p => (s.setKids p (replaceWith n (s.kidsOf n) (s.kidsOf p))).setKids n []

/-- Sorted by address, for comparing two states whatever order each was built in. -/
def normal (s : State) : State :=
  { objs := s.objs.mergeSort (fun x y => x.addr ≤ y.addr)
    caps := s.caps.mergeSort (fun x y => x.1 ≤ y.1)
    kids := (s.kids.filter (!·.2.isEmpty)).mergeSort (fun x y => x.1 ≤ y.1) }

/-- Up from a node, a root within as many steps as `fuel`. -/
def reachesRoot (s : State) (n : Nat) : Nat → Bool
  | 0 => false
  | fuel + 1 => match s.parent? n with
    | none => true
    | some p => s.reachesRoot p fuel

/--
The forest the trace claims: every child a filled node, in one list at most and there once,
every node with a list filled and with one list at most, and no node below itself.
`Forest` in `model/Rvuos/Forest.lean` says it of the lists, and `wellFormed_forest` that this checks it.
-/
def wellFormed (s : State) : Bool :=
  let all := s.kids.flatMap (·.2)
  let filled := all.all (fun k => (s.cap? k).isSome) && s.kids.all (fun p => (s.cap? p.1).isSome)
  let once := decide (s.kids.map (·.1)).Nodup && decide all.Nodup
  -- Up from each node, a root within as many steps as there are nodes.
  filled && once && s.caps.all (fun c => s.reachesRoot c.1 (s.caps.length + 1))

end State

/-! The words of an Untyped and a frame, in the 32 bits the kernel computes them in. -/

/-- `block_size`: the block's size from its word. -/
def blockSize (a : BitVec 32) : BitVec 32 := (a ^^^ (a + 1)) + 1

/-- `block_base`. -/
def blockBase (a : BitVec 32) : BitVec 32 := a &&& ~~~(blockSize a - 1)

/-- `block_word`: a block in one word, the bits below half its size set. -/
def blockWord (base size : BitVec 32) : BitVec 32 := base ||| (size / 2 - 1)

/-- `napot_block`: a power of two no smaller than the smallest region, aligned to itself. -/
def napot (regionMin : Nat) (base size : BitVec 32) : Bool :=
  size.toNat ≥ regionMin && (size &&& (size - 1)) == 0 && (base &&& (size - 1)) == 0

end Rvuos
