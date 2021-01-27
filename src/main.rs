use std::fs::File;
use std::io;
use std::process::exit;

use compat::attributes;
use plan::Plan;
use stable_eyre::eyre::{anyhow, Result};
use structopt::StructOpt;
use strum::{EnumString, IntoStaticStr};

use cas::Store;
use object::*;

mod cas;
mod compat;
mod execution;
mod object;
mod plan;

#[derive(StructOpt)]
enum Command {
    CasSmoke,
    RunJob {
        job_id: Id,
    },
    RunPlan {
        plan_path: String,
    },
    RunFlow {
        unit: String,
        params: Vec<String>,
    },
    ShowOutput {
        production_or_invocation: Id,
        path: String,
    },
    Print {
        objtype: ObjectType,
        id: Id,
    },
}

#[derive(Clone, Copy, EnumString, IntoStaticStr)]
enum ObjectType {
    #[strum(serialize = "invocation")]
    Invocation,
    #[strum(serialize = "production")]
    Production,
    #[strum(serialize = "job")]
    Job,
}

fn main() -> Result<()> {
    let command = Command::from_args();
    let mut store = cas::GitStore {};
    match command {
        Command::CasSmoke => {
            let cid = store.write("ignored", b"foo\n")?;
            println!("{}", cid);
            let mut obj = store.read("ignored", cid)?;
            io::copy(&mut obj, &mut io::stdout())?;
        }
        Command::RunJob { job_id } => {
            let production = execution::run_job(&mut store, job_id)?;
            attributes::to_writer(&mut io::stdout(), &production)?;
            if production.exit_code != 0 {
                exit(production.exit_code)
            }
        }
        Command::RunPlan { plan_path } => {
            let plan = Plan::from_reader(&mut File::open(plan_path)?)?;
            let invocation = execution::run_plan(&mut store, plan)?;
            let mut invocation_buf = Vec::new();
            attributes::to_writer(&mut invocation_buf, &invocation)?;
            let invocation_id = store.write("invocation", &invocation_buf)?;
            println!("{}", invocation_id);
            if invocation.status != InvocationStatus::Ok {
                exit(1);
            }
        }
        Command::RunFlow { unit, params } => {
            let invocation = execution::run_flow(&mut store, &unit, &params)?;
            let mut invocation_buf = Vec::new();
            attributes::to_writer(&mut invocation_buf, &invocation)?;
            let invocation_id = store.write("invocation", &invocation_buf)?;
            println!("{}", invocation_id);
            if invocation.status != InvocationStatus::Ok {
                eprintln!("fail: some jobs failed");
                exit(1);
            }
        }
        Command::ShowOutput {
            production_or_invocation,
            path,
        } => {
            // As a convenience, allow the user to specify an actual file in gen/out/,
            // since it will be easier to tab complete.
            let path = match path.strip_prefix("gen/") {
                Some(suffix) => suffix,
                None => &path,
            };

            let production_id = store
                .read("invocation", production_or_invocation)
                .and_then(|mut reader| Invocation::from_reader(&mut reader))
                .map_or(Some(production_or_invocation), |invocation| {
                    invocation.production
                })
                .ok_or_else(|| anyhow!("invocation missing production"))?;

            let production = store
                .read("production", production_id)
                .and_then(|mut reader| Production::from_reader(&mut reader))?;

            let mut reader = production
                .outputs
                .get(path)
                .ok_or_else(|| anyhow!("output not found"))
                .and_then(|&id| store.read("resource", id))?;
            io::copy(&mut reader, &mut io::stdout())?;
        }
        Command::Print { objtype, id } => {
            let mut reader = store.read(objtype.into(), id)?;
            match objtype {
                ObjectType::Invocation => {
                    let obj = Invocation::from_reader(&mut reader)?;
                    attributes::to_writer(&mut io::stdout(), &obj)?;
                }
                ObjectType::Production => {
                    let obj = Production::from_reader(&mut reader)?;
                    attributes::to_writer(&mut io::stdout(), &obj)?;
                }
                ObjectType::Job => {
                    let obj = Job::from_reader(&mut reader)?;
                    attributes::to_writer(&mut io::stdout(), &obj)?;
                }
            }
        }
    }

    Ok(())
}
