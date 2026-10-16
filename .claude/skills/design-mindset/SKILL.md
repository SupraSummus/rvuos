---
name: design-mindset
description: How to approach a design problem that looks like a forced trade-off, where every option seems to cost something that is ruled out (more memory per object, less reach, more work on a hot path, a limit a user can exhaust, less that a user can express). Use it before settling for a compromise in a data structure's layout, an encoding, an object model or an interface, and when weighing the options of an open design decision.
---

# The approach to a trade-off that looks forced

A trade-off that looks forced is usually a question asked too coarsely.
The habits below are a stance, not a procedure: take the ones the problem calls for.
Their examples are illustrations, not facts about any code; check the code itself for those.

## Habits

**Ask what is missing, not what is lost.**
A lost ability is missing information.
Say what the structure must know to do what it did, where that knowledge lived before, and where it lives now.
Every remedy stores it, recomputes it, or makes it unneeded,
and pays in bits, in memory, in time, in what a user can express, or in what a reader must follow;
naming which one an option pays in compares options that look unrelated.
A tree that stops storing a link to a node's parent, for example, must tell the parent some other way,
and whatever tells it is what dropping the link costs.

**Count the room case by case.**
"The record is full" is a total; the room is in the cases.
For each variant, and each place an instance can live, write down what each field holds.
Look for a word one variant leaves empty, low bits that alignment keeps zero,
codes an enumeration leaves unused, and freedom in where something may be placed.

**Re-read the last change for what it freed.**
A change made for one goal leaves room its author was not looking for.
After a change, list what it made unused or redundant, not only what it achieved:
a word it emptied may be exactly the room another problem lacks.

**Find what need not be stored.**
A leaf needs no link to children, a field that repeats another is free,
and what an address or a neighbour gives in constant time need not be kept.

**Hold the hard constraints fixed, and let rejections point.**
The hard constraints are the goals the project states and what the problem itself set out to keep;
write them down before comparing options.
An option that fits only by bending one of them is not a solution,
but it shows where the missing information wants to go.
A small counter a user can drive to its limit, for example, fails a rule against limits a user can exhaust,
and says the information has to live in the structure itself, exactly.

**Price the one exception honestly.**
When one case does not fit, shrink that case rather than bending the whole design around it,
and check how users actually use it before deciding:
narrowing a field costs little where every user passes it one kind of value.

**Prefer known meaning in a new representation.**
Changing how something is stored keeps what its specification, its models and its proofs say, and what its checks hold,
though their code follows the new layout;
changing what it means reopens all of them.
A new encoding of a structure whose meaning is known is worth more than a new structure that is almost as good.

**Let an experiment price an option.**
A branch that builds the option and runs the checks says what it costs better than an estimate,
and a check it fails names what the option cannot do.

**Know the prior art and what it gave up.**
Read how others solved the same problem, and the options already written down for this one,
and know why each works and what it costs, so that each can be borrowed in part.

**Think it through with whoever decides.**
Lay out the options with what each restores and what each costs, cheapest first,
and treat an objection as a new constraint that sharpens the search, not as a verdict on it.
Write the options down where the project keeps its open decisions, the rejected ones with why,
so that the next search starts where this one ended.

## Before settling for a compromise

- What exactly does the structure need to know, and where does it live now?
- Is the room counted per variant and per place, or only in total?
- What did the most recent change free up?
- What need not be stored: a link a leaf never uses, a field another repeats, what can be recomputed in constant time?
- Which of the hard constraints does each option bend, and what does that say?
- Is there a single case that does not fit, and what do users actually do with it?
- Does the option keep the existing meaning, and with it the specification, the proofs and what the checks hold?
- Has a branch measured what the option costs, or only an estimate?
- What did the prior art give up for the same saving, and which part of it can be borrowed?
- Has whoever decides seen each option with what it restores and costs, and are the rejected ones written down with why?
