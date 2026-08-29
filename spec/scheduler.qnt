// -*- mode: Bluespec; -*-
// Quint model of src/nix-eval-jobs.cc `Scheduler`.
//
//   quint run  spec/scheduler.qnt --main main --invariant inv --max-steps 150
//   quint test spec/scheduler.qnt --main noOomKiller
//
// Workers, todo/solo queues, Mode and memory are modelled. The JSON
// protocol is reduced to the events the scheduler reacts to. Each event
// is a guard `canX` and a transition `doX` on the whole State so the
// deadlock check can reuse the guards.
module scheduler {
  const N_WORKERS: int
  const BUDGET: int         // MiB
  const JOBS: Set[str]      // attribute names, children are not modelled
  const COST: str -> int    // peak growth of a job in MiB
  const BASE: int           // RSS of a fresh worker
  const EXTERNAL_KILLS: int // how often the kernel OOM killer may strike

  // PNone: no process. Killed: we SIGKILLed it, EOF pending.
  type Phase = PNone | Starting | Idle | Busy | Killed | Exiting
  type Mode = Parallel | Solo

  type Worker = {
    phase: Phase,
    job: str,       // "" when none
    rss: int,
    startRss: int,
    heap: int,      // retained after jobs, Boehm never shrinks
  }

  type State = {
    workers: int -> Worker,
    todo: Set[str],
    solo: List[str],
    mode: Mode,
    estimate: int,
    done: Set[str],
    errored: Set[str],
    externalKills: int,
  }

  pure val noWorker: Worker = { phase: PNone, job: "", rss: 0, startRss: 0, heap: 0 }
  pure val freshWorker: Worker = { ...noWorker, phase: Starting, rss: BASE, heap: BASE }
  pure val ids: Set[int] = 0.to(N_WORKERS - 1)
  pure def imax(a: int, b: int): int = if (a > b) a else b

  // ---- derived --------------------------------------------------------
  pure def alive(w: Worker): bool = w.phase != PNone
  pure def running(w: Worker): bool = w.phase == Busy or w.phase == Killed
  pure def peak(w: Worker): int = w.heap + (if (w.job == "") 0 else COST.get(w.job))

  pure def w(s: State, i: int): Worker = s.workers.get(i)
  pure def setW(s: State, i: int, x: Worker): State = { ...s, workers: s.workers.set(i, x) }
  pure def count(s: State, f: Worker => bool): int = ids.filter(i => f(s.w(i))).size()
  pure def nRunning(s: State): int = s.count(running)
  pure def nAlive(s: State): int = s.count(alive)
  pure def nPhase(s: State, p: Phase): int = s.count(x => x.phase == p)
  pure def totalRss(s: State): int = ids.fold(0, (acc, i) => acc + s.w(i).rss)
  pure def finished(s: State): bool = s.todo == Set() and length(s.solo) == 0 and s.nRunning() == 0
  pure def terminated(s: State): bool = s.finished() and s.nAlive() == 0
  pure def fitsBudget(s: State): bool =
    s.nRunning() == 0 or s.totalRss() + (s.nRunning() + 1) * s.estimate <= BUDGET
  // In Solo mode exactly one worker may exist at all.
  pure def spawnable(s: State): int =
    if (s.mode == Solo) (if (s.nAlive() > 0) 0 else 1)
    else if (length(s.solo) == 0) s.todo.size() - (s.nPhase(Starting) + s.nPhase(Idle))
    else 0

  // ---- dispatch() -----------------------------------------------------
  pure def canSpawn(s: State, i: int): bool =
    not(s.finished()) and s.w(i).phase == PNone and s.spawnable() > 0
  pure def doSpawn(s: State, i: int): State = s.setW(i, freshWorker)

  pure def canSendExit(s: State, i: int): bool = s.finished() and s.w(i).phase == Idle
  pure def doSendExit(s: State, i: int): State = s.setW(i, { ...s.w(i), phase: Exiting })

  pure def canEnterSolo(s: State): bool =
    not(s.finished()) and s.mode == Parallel and length(s.solo) > 0 and s.nRunning() == 0
  pure def doEnterSolo(s: State): State =
    { ...s, workers: ids.mapBy(_ => noWorker), mode: Solo }

  pure def canTakeSolo(s: State, i: int): bool =
    not(s.finished()) and s.w(i).phase == Idle and s.mode == Solo and
    s.nRunning() == 0 and length(s.solo) > 0
  pure def doTakeSolo(s: State, i: int): State =
    if (length(s.solo) == 0) s else {
      val job = s.solo[length(s.solo) - 1]
      { ...s.setW(i, { ...s.w(i), phase: Busy, job: job, startRss: s.w(i).rss }),
        solo: s.solo.slice(0, length(s.solo) - 1) }
    }

  pure def canTakeTodo(s: State, i: int): bool =
    not(s.finished()) and s.w(i).phase == Idle and s.mode == Parallel and
    length(s.solo) == 0 and s.todo != Set() and s.fitsBudget()
  pure def doTakeTodo(s: State, i: int, job: str): State =
    { ...s.setW(i, { ...s.w(i), phase: Busy, job: job, startRss: s.w(i).rss }),
      todo: s.todo.exclude(Set(job)) }

  // ---- worker output --------------------------------------------------
  // "next"
  pure def canStarted(s: State, i: int): bool = s.w(i).phase == Starting
  pure def doStarted(s: State, i: int): State = s.setW(i, { ...s.w(i), phase: Idle })

  // "restart": rss > max-memory-size after a job
  pure def canRestarted(s: State, i: int): bool =
    s.w(i).phase == Starting and s.w(i).heap > BUDGET / N_WORKERS
  pure def doRestarted(s: State, i: int): State = s.setW(i, noWorker)

  // response line
  pure def canFinish(s: State, i: int): bool = s.w(i).phase == Busy and s.w(i).rss == s.w(i).peak()
  pure def doFinish(s: State, i: int): State = {
    val x = s.w(i)
    val retained = x.heap + (x.peak() - x.heap) / 2
    { ...s.setW(i, { ...noWorker, phase: Starting, heap: retained, rss: retained }),
      estimate: imax(s.estimate * 98 / 100, retained - x.startRss),
      done: s.done.union(Set(x.job)),
      mode: Parallel }
  }

  // EOF from an Exiting worker
  pure def canExited(s: State, i: int): bool = s.w(i).phase == Exiting
  pure def doExited(s: State, i: int): State = s.setW(i, noWorker)

  // EOF + SIGKILL, by us (Killed) or the kernel (any live, non-exiting worker)
  pure def canKilledEof(s: State, i: int): bool =
    s.w(i).phase == Killed or
    (alive(s.w(i)) and s.w(i).phase != Exiting and s.externalKills < EXTERNAL_KILLS)
  pure def doKilledEof(s: State, i: int): State = {
    val x = s.w(i)
    val s1 = { ...s.setW(i, noWorker),
               mode: Parallel,
               externalKills: if (x.phase == Killed) s.externalKills else s.externalKills + 1 }
    if (not(running(x))) s1
    else if (s.mode == Solo) { ...s1, errored: s.errored.union(Set(x.job)) }
    else { ...s1, solo: s.solo.append(x.job) }
  }

  // ---- time passing ---------------------------------------------------
  pure def canGrow(s: State, i: int): bool = s.w(i).phase == Busy and s.w(i).rss < s.w(i).peak()
  pure def doGrow(s: State, i: int): State = s.setW(i, { ...s.w(i), rss: s.w(i).peak() })

  // sampleMemory(): learn estimate from in-flight growth, evict largest Busy if over budget
  pure def canSample(s: State): bool = s.nAlive() > 0
  pure def doSample(s: State, victim: int): State = {
    val est = ids.fold(s.estimate, (e, i) =>
      if (running(s.w(i))) imax(e, s.w(i).rss - s.w(i).startRss) else e)
    val s1 = { ...s, estimate: est }
    if (s.totalRss() > BUDGET and s.w(victim).phase == Busy)
      s1.setW(victim, { ...s.w(victim), phase: Killed })
    else s1
  }
  pure def isLargestBusy(s: State, v: int): bool =
    s.w(v).phase == Busy and ids.forall(k => s.w(k).phase != Busy or s.w(k).rss <= s.w(v).rss)

  // ---- state machine --------------------------------------------------
  var st: State
  var wasFinished: bool // ghost

  action init = all {
    st' = { workers: ids.mapBy(_ => noWorker), todo: JOBS, solo: [], mode: Parallel,
            estimate: 512, done: Set(), errored: Set(), externalKills: 0 },
    wasFinished' = false,
  }

  action guarded(g: bool, s1: State): bool = all { g, st' = s1, wasFinished' = st.finished() }

  action step = {
    nondet i = ids.oneOf()
    nondet job = st.todo.union(Set("")).oneOf()
    nondet victim = ids.oneOf()
    any {
      guarded(st.canSpawn(i),     st.doSpawn(i)),
      guarded(st.canSendExit(i),  st.doSendExit(i)),
      guarded(st.canEnterSolo(),  st.doEnterSolo()),
      guarded(st.canTakeSolo(i),  st.doTakeSolo(i)),
      guarded(st.canTakeTodo(i) and job != "", st.doTakeTodo(i, job)),
      guarded(st.canStarted(i),   st.doStarted(i)),
      guarded(st.canRestarted(i), st.doRestarted(i)),
      guarded(st.canFinish(i),    st.doFinish(i)),
      guarded(st.canExited(i),    st.doExited(i)),
      guarded(st.canKilledEof(i), st.doKilledEof(i)),
      guarded(st.canGrow(i),      st.doGrow(i)),
      guarded(st.canSample() and (st.isLargestBusy(victim) or st.nPhase(Busy) == 0),
              st.doSample(victim)),
    }
  }

  action stepOrStutter = any {
    step,
    all { st.terminated(), st' = st, wasFinished' = wasFinished },
  }

  // ---- properties -----------------------------------------------------
  val runningJobs = ids.filter(i => running(st.w(i))).map(i => st.w(i).job)
  val soloJobs = st.solo.indices().map(k => st.solo[k])

  // Every job is in exactly one place.
  val conservation =
    JOBS == st.todo.union(soloJobs).union(runningJobs).union(st.done).union(st.errored) and
    JOBS.size() == st.todo.size() + length(st.solo) + st.nRunning() + st.done.size() + st.errored.size()

  // Solo means one fresh worker and nothing else.
  val soloAlone = st.mode == Solo implies (
    st.nAlive() <= 1 and
    ids.forall(i => running(st.w(i)) implies st.w(i).heap == BASE))

  // Solo is left as soon as its job ends, so it always has work.
  val soloProgress = st.mode == Solo implies (length(st.solo) > 0 or st.nRunning() == 1)

  // Workers are only told to exit once nothing can produce work anymore.
  val exitingOnlyWhenFinished =
    (st.nPhase(Exiting) > 0 implies st.finished()) and (wasFinished implies st.finished())

  val phasesConsistent = ids.forall(i => running(st.w(i)) == (st.w(i).job != ""))

  // `while (!finished || anyAlive) poll()` never blocks forever.
  val someEnabled = st.terminated() or st.canEnterSolo() or ids.exists(i =>
    st.canSpawn(i) or st.canSendExit(i) or st.canTakeSolo(i) or st.canTakeTodo(i) or
    st.canStarted(i) or st.canRestarted(i) or st.canFinish(i) or st.canExited(i) or
    st.w(i).phase == Killed or st.canGrow(i))

  // Once everything terminated, every job has a verdict.
  val complete = st.terminated() implies st.done.union(st.errored) == JOBS

  val inv = conservation and soloAlone and soloProgress and exitingOnlyWhenFinished and
            phasesConsistent and someEnabled and complete

  // No livelock: any interleaving terminates within the step bound.
  run terminatesTest = init.then(300.reps(_ => stepOrStutter)).expect(st.terminated())

  // Without the kernel OOM killer only a job larger than the whole
  // budget can end up as an error.
  run onlyHugeErrorsTest = init.then(300.reps(_ => stepOrStutter))
    .expect(st.errored.subseteq(JOBS.filter(j => COST.get(j) + BASE > BUDGET)))
}

module main {
  import scheduler(
    N_WORKERS = 3, BUDGET = 3000, BASE = 300, EXTERNAL_KILLS = 2,
    JOBS = Set("a", "b", "c", "fat", "huge"),
    COST = Map("a" -> 100, "b" -> 400, "c" -> 800, "fat" -> 2000, "huge" -> 5000),
  ).*
}

module noOomKiller {
  import scheduler(
    N_WORKERS = 3, BUDGET = 3000, BASE = 300, EXTERNAL_KILLS = 0,
    JOBS = Set("a", "b", "c", "fat", "huge"),
    COST = Map("a" -> 100, "b" -> 400, "c" -> 800, "fat" -> 2000, "huge" -> 5000),
  ).*
}
