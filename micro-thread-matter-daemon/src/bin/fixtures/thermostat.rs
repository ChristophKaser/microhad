//! Synthetic thermostat only; never connected to a physical accessory.
use micro_thread_matter_daemon::thermostat::{Schedule, Transition};
use rs_matter::dm::clusters::decl::thermostat::*;
use rs_matter::{
    dm::{Cluster, InvokeContext, ReadContext, WriteContext},
    error::{Error, ErrorCode},
    tlv::{Nullable, TLVBuilderParent},
};
use std::cell::{Cell, RefCell};
pub struct TestThermostat {
    pub temperature: Cell<Option<i16>>,
    heat: Cell<i16>,
    cool: Cell<i16>,
    mode: Cell<SystemModeEnum>,
    dataver: Cell<u32>,
    schedules: RefCell<[Schedule; 7]>,
}
impl TestThermostat {
    pub fn new() -> Self {
        Self {
            temperature: Cell::new(Some(2150)),
            heat: Cell::new(2000),
            cool: Cell::new(2600),
            mode: Cell::new(SystemModeEnum::Off),
            dataver: Cell::new(1),
            schedules: RefCell::new(std::array::from_fn(|_| Schedule {
                mode: 3,
                transitions: vec![Transition {
                    time: 0,
                    heat: Some(20.0),
                    cool: Some(26.0),
                }],
            })),
        }
    }
}
impl ClusterHandler for TestThermostat {
    const CLUSTER: Cluster<'static> = FULL_CLUSTER.with_features(1|2|8|32).with_attrs(rs_matter::with!(required;
        AttributeId::OccupiedHeatingSetpoint | AttributeId::OccupiedCoolingSetpoint |
        AttributeId::AbsMinHeatSetpointLimit | AttributeId::AbsMaxHeatSetpointLimit |
        AttributeId::AbsMinCoolSetpointLimit | AttributeId::AbsMaxCoolSetpointLimit |
        AttributeId::MinHeatSetpointLimit | AttributeId::MaxHeatSetpointLimit |
        AttributeId::MinCoolSetpointLimit | AttributeId::MaxCoolSetpointLimit |
        AttributeId::MinSetpointDeadBand | AttributeId::NumberOfDailyTransitions | AttributeId::NumberOfWeeklyTransitions
    )).with_cmds(rs_matter::with!(CommandId::SetWeeklySchedule | CommandId::GetWeeklySchedule));
    fn dataver(&self) -> u32 {
        self.dataver.get()
    }
    fn dataver_changed(&self) {
        self.dataver.set(self.dataver.get().wrapping_add(1));
    }
    fn local_temperature(&self, _: impl ReadContext) -> Result<Nullable<i16>, Error> {
        Ok(self.temperature.get().into())
    }
    fn control_sequence_of_operation(
        &self,
        _: impl ReadContext,
    ) -> Result<ControlSequenceOfOperationEnum, Error> {
        Ok(ControlSequenceOfOperationEnum::CoolingAndHeating)
    }
    fn set_control_sequence_of_operation(
        &self,
        _: impl WriteContext,
        _: ControlSequenceOfOperationEnum,
    ) -> Result<(), Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn system_mode(&self, _: impl ReadContext) -> Result<SystemModeEnum, Error> {
        Ok(self.mode.get())
    }
    fn set_system_mode(&self, _: impl WriteContext, value: SystemModeEnum) -> Result<(), Error> {
        self.mode.set(value);
        Ok(())
    }
    fn occupied_heating_setpoint(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(self.heat.get())
    }
    fn occupied_cooling_setpoint(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(self.cool.get())
    }
    fn set_occupied_heating_setpoint(&self, _: impl WriteContext, value: i16) -> Result<(), Error> {
        self.heat.set(value);
        Ok(())
    }
    fn set_occupied_cooling_setpoint(&self, _: impl WriteContext, value: i16) -> Result<(), Error> {
        self.cool.set(value);
        Ok(())
    }
    fn abs_min_heat_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(500)
    }
    fn abs_max_heat_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(3500)
    }
    fn abs_min_cool_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(1000)
    }
    fn abs_max_cool_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(4000)
    }
    fn min_heat_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(700)
    }
    fn max_heat_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(3000)
    }
    fn min_cool_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(1600)
    }
    fn max_cool_setpoint_limit(&self, _: impl ReadContext) -> Result<i16, Error> {
        Ok(3200)
    }
    fn min_setpoint_dead_band(&self, _: impl ReadContext) -> Result<i8, Error> {
        Ok(25)
    }
    fn number_of_daily_transitions(&self, _: impl ReadContext) -> Result<u8, Error> {
        Ok(10)
    }
    fn number_of_weekly_transitions(&self, _: impl ReadContext) -> Result<u8, Error> {
        Ok(70)
    }
    fn handle_set_weekly_schedule(
        &self,
        _: impl InvokeContext,
        request: SetWeeklyScheduleRequest<'_>,
    ) -> Result<(), Error> {
        let mode = request.mode_for_sequence()?.bits();
        let mut transitions = Vec::new();
        for row in request.transitions()?.iter() {
            let row = row?;
            transitions.push(Transition {
                time: row.transition_time()?,
                heat: if mode & 1 != 0 {
                    row.heat_setpoint()?
                        .as_opt_ref()
                        .map(|v| f64::from(*v) / 100.0)
                } else {
                    None
                },
                cool: if mode & 2 != 0 {
                    row.cool_setpoint()?
                        .as_opt_ref()
                        .map(|v| f64::from(*v) / 100.0)
                } else {
                    None
                },
            });
        }
        for day in 0..7 {
            if request.day_of_week_for_sequence()?.bits() & (1 << day) != 0 {
                self.schedules.borrow_mut()[day] = Schedule {
                    mode,
                    transitions: transitions.clone(),
                };
            }
        }
        Ok(())
    }
    fn handle_get_weekly_schedule<P: TLVBuilderParent>(
        &self,
        _: impl InvokeContext,
        request: GetWeeklyScheduleRequest<'_>,
        response: GetWeeklyScheduleResponseBuilder<P>,
    ) -> Result<P, Error> {
        let days = request.days_to_return()?.bits();
        let day = (0..7)
            .find(|day| days & (1 << day) != 0)
            .ok_or(ErrorCode::InvalidData)?;
        let schedules = self.schedules.borrow();
        let schedule = &schedules[day];
        let mode = schedule.mode & request.mode_to_return()?.bits();
        let mut entries = response
            .number_of_transitions_for_sequence(schedule.transitions.len() as u8)?
            .day_of_week_for_sequence(ScheduleDayOfWeekBitmap::from_bits_retain(1 << day))?
            .mode_for_sequence(ScheduleModeBitmap::from_bits_retain(mode))?
            .transitions()?;
        for t in &schedule.transitions {
            entries = entries
                .push()?
                .transition_time(t.time)?
                .heat_setpoint(if mode & 1 != 0 {
                    t.heat.map(|v| (v * 100.0).round() as i16).into()
                } else {
                    Nullable::none()
                })?
                .cool_setpoint(if mode & 2 != 0 {
                    t.cool.map(|v| (v * 100.0).round() as i16).into()
                } else {
                    Nullable::none()
                })?
                .end()?;
        }
        entries.end()?.end()
    }
    fn handle_setpoint_raise_lower(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
        _request: SetpointRaiseLowerRequest<'_>,
    ) -> Result<(), rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn handle_clear_weekly_schedule(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
    ) -> Result<(), rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn handle_set_active_schedule_request(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
        _request: SetActiveScheduleRequestRequest<'_>,
    ) -> Result<(), rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn handle_set_active_preset_request(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
        _request: SetActivePresetRequestRequest<'_>,
    ) -> Result<(), rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn handle_add_thermostat_suggestion<P: rs_matter::tlv::TLVBuilderParent>(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
        _request: AddThermostatSuggestionRequest<'_>,
        _response: AddThermostatSuggestionResponseBuilder<P>,
    ) -> Result<P, rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn handle_remove_thermostat_suggestion(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
        _request: RemoveThermostatSuggestionRequest<'_>,
    ) -> Result<(), rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
    fn handle_atomic_request<P: rs_matter::tlv::TLVBuilderParent>(
        &self,
        _ctx: impl rs_matter::dm::InvokeContext,
        _request: AtomicRequestRequest<'_>,
        _response: AtomicResponseBuilder<P>,
    ) -> Result<P, rs_matter::error::Error> {
        Err(ErrorCode::InvalidAction.into())
    }
}
