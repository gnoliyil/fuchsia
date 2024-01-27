// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::{MathFunction, MetricValue, Problem};

enum PromotedOperands {
    Float(Vec<f64>),
    Int(Vec<i64>),
}

// TODO(fxbug.dev/57073): More informative error messages as part of structured errors.

pub fn calculate(function: &MathFunction, operands: &Vec<MetricValue>) -> MetricValue {
    // Arity check. + and * are well-defined for 1..N arguments, but the parser will only
    // give us 2 arguments. This check avoids panics from internal bugs.
    match function {
        MathFunction::Min | MathFunction::Max if operands.len() > 0 => {}
        MathFunction::Min | MathFunction::Max => {
            return super::syntax_error("No operands in math expression");
        }
        MathFunction::Abs if operands.len() == 1 => {}
        MathFunction::Abs => return super::syntax_error("Abs requires exactly one operand."),
        _ if operands.len() == 2 => {}
        _ => return super::internal_bug("Internal bug. Function needs 2 arguments."),
    }
    let operands = match promote_type(operands) {
        Ok(operands) => operands,
        Err(value) => return value,
    };
    match operands {
        PromotedOperands::Float(operands) => MetricValue::Float(match function {
            MathFunction::Add => operands[0] + operands[1],
            MathFunction::Sub => operands[0] - operands[1],
            MathFunction::Mul => operands[0] * operands[1],
            MathFunction::FloatDiv | MathFunction::IntDiv if operands[1] == 0.0 => {
                return super::value_error("Division by zero")
            }
            MathFunction::FloatDivChecked | MathFunction::IntDivChecked if operands[1] == 0.0 => {
                return MetricValue::Problem(Problem::Ignore(vec![Problem::ValueError(
                    "Division by zero".to_string(),
                )]))
            }
            MathFunction::FloatDiv | MathFunction::FloatDivChecked => operands[0] / operands[1],
            MathFunction::IntDiv | MathFunction::IntDivChecked => {
                return match super::safe_float_to_int(operands[0] / operands[1]) {
                    Some(int) => MetricValue::Int(int),
                    None => super::value_error("Non-numeric division result"),
                }
            }
            MathFunction::Greater => return MetricValue::Bool(operands[0] > operands[1]),
            MathFunction::Less => return MetricValue::Bool(operands[0] < operands[1]),
            MathFunction::GreaterEq => return MetricValue::Bool(operands[0] >= operands[1]),
            MathFunction::LessEq => return MetricValue::Bool(operands[0] <= operands[1]),
            MathFunction::Min => fold(operands, &f64::min),
            MathFunction::Max => fold(operands, &f64::max),
            MathFunction::Abs => operands[0].abs(),
        }),
        PromotedOperands::Int(operands) => MetricValue::Int(match function {
            MathFunction::Add => operands[0] + operands[1],
            MathFunction::Sub => operands[0] - operands[1],
            MathFunction::Mul => operands[0] * operands[1],
            MathFunction::FloatDiv | MathFunction::IntDiv if operands[1] == 0 => {
                return super::value_error("Division by zero")
            }
            MathFunction::FloatDivChecked | MathFunction::IntDivChecked if operands[1] == 0 => {
                return MetricValue::Problem(Problem::Ignore(vec![Problem::ValueError(
                    "Division by zero".to_string(),
                )]))
            }
            MathFunction::FloatDiv | MathFunction::FloatDivChecked => {
                return MetricValue::Float(operands[0] as f64 / operands[1] as f64)
            }
            MathFunction::IntDiv | MathFunction::IntDivChecked => operands[0] / operands[1],
            MathFunction::Greater => return MetricValue::Bool(operands[0] > operands[1]),
            MathFunction::Less => return MetricValue::Bool(operands[0] < operands[1]),
            MathFunction::GreaterEq => return MetricValue::Bool(operands[0] >= operands[1]),
            MathFunction::LessEq => return MetricValue::Bool(operands[0] <= operands[1]),
            MathFunction::Min => fold(operands, &i64::min),
            MathFunction::Max => fold(operands, &i64::max),
            MathFunction::Abs => operands[0].abs(),
        }),
    }
}

fn fold<T: num_traits::Num + Copy>(operands: Vec<T>, function: &dyn (Fn(T, T) -> T)) -> T {
    let mut iter = operands.iter();
    let mut result = *iter.next().unwrap(); // Checked non-empty in calculate()
    loop {
        match iter.next() {
            Some(next) => result = function(result, *next),
            None => return result,
        }
    }
}

fn promote_type(operands: &Vec<MetricValue>) -> Result<PromotedOperands, MetricValue> {
    let mut int_vec = Vec::with_capacity(operands.len());
    let mut float_vec = Vec::with_capacity(operands.len());
    let mut error_vec = Vec::with_capacity(operands.len());
    let mut non_numeric_error = None;
    for o in operands.iter() {
        match super::unwrap_for_math(o) {
            MetricValue::Int(value) => {
                int_vec.push(*value);
                float_vec.push(*value as f64);
            }
            MetricValue::Float(value) => {
                float_vec.push(*value);
            }
            MetricValue::Problem(problem) => {
                error_vec.push(problem);
            }
            bad_type => {
                non_numeric_error = Some(Problem::Missing(format!("{} not numeric", bad_type)));
            }
        }
    }
    if int_vec.len() == operands.len() {
        return Ok(PromotedOperands::Int(int_vec));
    }
    if float_vec.len() == operands.len() {
        return Ok(PromotedOperands::Float(float_vec));
    }
    if non_numeric_error.is_some() {
        error_vec.push(&non_numeric_error.as_ref().unwrap());
    }
    return Err(MetricValue::Problem(super::MetricState::important_problem(error_vec)));
}

// Correct operation of this file is tested in parse.rs.
