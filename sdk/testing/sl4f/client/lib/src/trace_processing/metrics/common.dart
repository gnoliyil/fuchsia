// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// @dart=2.12

import 'dart:math';
import 'package:collection/collection.dart' show IterableNullableExtension;

import '../time_delta.dart';
import '../time_point.dart';
import '../trace_model.dart';

// This file contains common utility functions that are shared across metrics.

T _add<T extends num>(T a, T b) => a + b as T;
T _min<T extends Comparable<T>>(T a, T b) => a.compareTo(b) < 0 ? a : b;
T _max<T extends Comparable<T>>(T a, T b) => a.compareTo(b) > 0 ? a : b;

/// Get all events contained in [model], without regard for what [Process] or
/// [Thread] they belong to.
Iterable<Event> getAllEvents(Model model) => model.processes
    .expand((process) => process.threads.expand((thread) => thread.events));

/// Get the trace duration by subtracing the max event end time by the min event start time.
TimeDelta getTotalTraceDuration(Model model) {
  final allEvents = getAllEvents(model);
  if (allEvents.isEmpty) {
    return TimeDelta.zero();
  }
  final TimePoint traceStartTime = allEvents.map((e) => e.start).reduce(_min);
  final TimePoint traceEndTime = allEvents
      .map((e) => e is DurationEvent
          ? e.start + e.duration!
          : (e is AsyncEvent ? e.start + e.duration! : e.start))
      .reduce(_max);
  return traceEndTime - traceStartTime;
}

/// Filter [events] for events that have a matching [category] and [name]
/// fields.
Iterable<Event> filterEvents(Iterable<Event> events,
        {String? category, String? name}) =>
    events.where((event) =>
        (category == null || event.category == category) &&
        (name == null || event.name == name));

/// Filter [events] for events that have a matching [category] and [name]
/// fields, that are also of type [T].
Iterable<T> filterEventsTyped<T extends Event>(Iterable<Event> events,
        {String? category, String? name}) =>
    Iterable.castFrom<Event, T>(events.where((event) =>
        (event is T) &&
        (category == null || event.category == category) &&
        (name == null || event.name == name)));

Iterable<T> getArgValuesFromEvents<T extends Object>(
    Iterable<Event> events, String argKey) {
  return events.map((e) {
    if (!(e.args.containsKey(argKey) && e.args[argKey] is T)) {
      throw ArgumentError(
          'Error, expected events to include arg key "$argKey" of type $T');
    }
    final T v = e.args[argKey]!;
    return v;
  });
}

/// Find all [Event]s that follow [event].
///
/// A following event is defined to be all events that are reachable by walking
/// forward in the flow/duration graph.  "Walking forward" means recursively
/// visiting all sub-duration-events and outgoing flow events for duration
/// events, and the enclosing duration and next flow event for flow events.
List<Event> getFollowingEvents(Event event) {
  final List<Event?> frontier = [event];
  final Set<Event> visited = {};

  while (frontier.isNotEmpty) {
    final current = frontier.removeLast();
    if (current == null) {
      continue;
    }
    final added = visited.add(current);
    if (!added) {
      continue;
    }
    if (current is DurationEvent) {
      frontier
        ..addAll(current.childDurations)
        ..addAll(current.childFlows);
    } else if (current is FlowEvent) {
      frontier
        ..add(current.enclosingDuration)
        ..add(current.nextFlow);
    } else {
      assert(false);
    }
  }

  final result = List<Event>.from(visited)
    ..sort((a, b) => a.start.compareTo(b.start));
  return result;
}

/// Find the first "VSYNC" event that [durationEvent] is connected to.
///
/// Returns [null] if no "VSYNC" is found.
DurationEvent? findFollowingVsync(DurationEvent durationEvent) {
  final followingVsyncs = filterEventsTyped<DurationEvent>(
      getFollowingEvents(durationEvent),
      category: 'gfx',
      name: 'Display::Controller::OnDisplayVsync');
  if (followingVsyncs.isNotEmpty) {
    return followingVsyncs.first;
  }
  return null;
}

/// Returns a list of durations in milliseconds from the given |startEventName|
/// events in |startEventCategory| to the nearest connected  "VSYNC" events.
List<double> getEventToVsyncLatencyValues(
    Model model, String startEventCategory, String startEventName) {
  final startEvents = filterEventsTyped<DurationEvent>(getAllEvents(model),
      category: startEventCategory, name: startEventName);
  final vsyncEvents = startEvents.map(findFollowingVsync);

  return Zip2Iterable<DurationEvent, DurationEvent?, double?>(
          startEvents,
          vsyncEvents,
          (startEvent, vsyncEvent) => (vsyncEvent == null)
              ? null
              : (vsyncEvent.start - startEvent.start).toMillisecondsF())
      .whereNotNull()
      .toList();
}

/// Compute the mean (https://en.wikipedia.org/wiki/Arithmetic_mean#Definition)
/// of [values].
double computeMean<T extends num>(Iterable<T> values) {
  if (values.isEmpty) {
    throw ArgumentError(
        '[values] must not be empty in order to compute its average');
  }
  return values.reduce(_add) / values.length;
}

/// Compute the population variance (https://en.wikipedia.org/wiki/Variance#Population_variance)
/// of [values].
double computeVariance<T extends num>(Iterable<T> values) {
  final mean = computeMean(values);
  return values.map((value) => pow(value - mean, 2.0)).reduce(_add) /
      values.length;
}

/// Compute the population standard deviation (https://en.wikipedia.org/wiki/Standard_deviation#Uncorrected_sample_standard_deviation)
/// of [values].
double computeStandardDeviation<T extends num>(Iterable<T> values) =>
    sqrt(computeVariance(values));

/// Compute the linear interpolated [percentile]th percentile
///  (https://en.wikipedia.org/wiki/Percentile) of [values].
double computePercentile<T extends num>(Iterable<T> values, int percentile) {
  if (values.isEmpty) {
    throw ArgumentError(
        '[values] must not be empty in order to compute percentile');
  }
  final valuesAsList = values.toList()..sort();
  if (percentile == 100) {
    return valuesAsList.last.toDouble();
  }
  final indexAsFloat = (valuesAsList.length - 1.0) * (0.01 * percentile);
  final index = indexAsFloat.floor();
  final fractional = indexAsFloat % 1.0;
  if (index + 1 == valuesAsList.length) {
    return valuesAsList.last.toDouble();
  }
  return valuesAsList[index] * (1.0 - fractional) +
      valuesAsList[index + 1] * fractional;
}

/// Compute the maximum (https://en.wikipedia.org/wiki/Sample_maximum_and_minimum)
/// of [values].
T computeMax<T extends num>(Iterable<T> values) => values.reduce(max);

/// Compute the minimum (# https://en.wikipedia.org/wiki/Sample_maximum_and_minimum)
/// of [values].
T computeMin<T extends num>(Iterable<T> values) => values.reduce(min);

/// Compute the 1st degree difference of [values].
///
/// I.e., [x0, x1, x2, ...] -> [(x1 - x0), (x2 - x1), ...]
List<T> differenceValues<T extends num>(Iterable<T> values) {
  final List<T> result = [];
  T? previous;
  for (final value in values) {
    if (previous != null) {
      final difference = value - previous;
      result.add(difference as T);
    }
    previous = value;
  }
  return result;
}

/// Generate a string summary of [values].
///
/// Example output:
///   count: 1033
///   mean:  1.5378480048402718
///   std:   1.5095374570697047
///   min:   0.626416
///   25%:   1.105875
///   50%:   1.173375
///   75%:   1.3925
///   max:   26.257833
String describeValues<T extends num>(List<T> values,
        {int indent = 0, List<int> percentiles = const [25, 50, 75]}) =>
    [
      'count: ${values.length}',
      'mean:  ${values.isNotEmpty ? computeMean(values) : double.nan}',
      'std:   ${values.length > 1 ? computeStandardDeviation(values) : double.nan}',
      'min:   ${values.isNotEmpty ? computeMin(values) : double.nan}',
      ...percentiles.map((percentile) {
        final statName = '$percentile%';
        final lineStart = '$statName:${' ' * (6 - statName.length)}';
        return '$lineStart${values.isNotEmpty ? computePercentile(values, percentile) : double.nan}';
      }),
      'max:   ${values.isNotEmpty ? computeMax(values) : double.nan}',
      '',
    ].map((line) => '${' ' * indent}$line\n').join('');

/// Iterate over a sequence of [T1]s and [T2]s by applying [_f] to pairs of
/// elements in the sequences.
///
/// In other words, iterate over [x1, x2, ...] and [y1, y2, ...] like
/// [_f(x1, y1), _f(x2, y2), ...].  Iteration stops when any [Iterable] is
/// exhausted.
class Zip2Iterable<T1, T2, R> extends Iterable<R?> {
  final Iterable<T1> _t1s;
  final Iterable<T2> _t2s;
  final R Function(T1, T2) _f;

  Zip2Iterable(this._t1s, this._t2s, this._f);

  @override
  Iterator<R?> get iterator =>
      Zip2Iterator<T1, T2, R>(_t1s.iterator, _t2s.iterator, _f);
}

class Zip2Iterator<T1, T2, R> extends Iterator<R?> {
  final Iterator<T1> _t1s;
  final Iterator<T2> _t2s;
  final R Function(T1, T2) _f;

  R? _current;

  Zip2Iterator(this._t1s, this._t2s, this._f);

  @override
  bool moveNext() {
    if (!(_t1s.moveNext() && _t2s.moveNext())) {
      return false;
    }
    _current = _f(_t1s.current, _t2s.current);
    return true;
  }

  @override
  R? get current => _current;
}
