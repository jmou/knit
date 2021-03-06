use std::fs::File;
use std::io;
use std::process::exit;

use stable_eyre::eyre::{anyhow, Result};
use structopt::StructOpt;
use strum::{EnumString, IntoStaticStr};

use cas::UntypedId;
use compat::attributes;
use compat::cas::GitStore;
use compat::context::{DirectoryResourceAccessor, RealEnvironment};
use object::*;
use plan::TextPlan;

mod cas;
mod compat;
mod execution;
mod object;
mod plan;

#[derive(StructOpt)]
enum Command {
    RunJob {
        job_id: Id<Job>,
    },
    RunPlan {
        plan_path: String,
        dir: Option<String>,
    },
    ShowOutput {
        production_or_invocation: UntypedId,
        path: Option<String>,
    },
    Print {
        objtype: ObjectType,
        id: UntypedId,
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
    stable_eyre::install()?;

    let command = Command::from_args();
    let store = cas::Store(Box::new(GitStore {}));
    let env = RealEnvironment::new(&store);
    match command {
        Command::RunJob { job_id } => {
            let production = execution::run_job(&env, &job_id)?;
            attributes::to_writer(&mut io::stdout(), &production)?;
            if production.exit_code != 0 {
                exit(production.exit_code)
            }
        }
        Command::RunPlan { plan_path, dir } => {
            let accessor = DirectoryResourceAccessor::new(dir.as_deref().unwrap_or(""), &store);
            let plan = TextPlan::from_reader(Box::new(File::open(plan_path)?))?;
            let plan_id = store.write_resource(&plan.to_bytes())?;
            let plan = plan.encode(&accessor, &store)?;
            let invocation = execution::run_plan(&env, plan, &plan_id)?;
            let invocation_id = store.write(&invocation)?;
            println!("{}", invocation_id);
            if invocation.status != InvocationStatus::Ok {
                exit(1);
            }
        }
        Command::ShowOutput {
            production_or_invocation,
            path,
        } => {
            let path = path.as_deref().unwrap_or("out/");
            // As a convenience, allow the user to specify an actual file in gen/out/,
            // since it will be easier to tab complete.
            let path = match path.strip_prefix("gen/") {
                Some(suffix) => suffix,
                None => &path,
            };

            let production_id = store
                .read(&Id::<Invocation>::new(production_or_invocation))
                .map_or_else(
                    |_| Some(Id::<Production>::new(production_or_invocation)),
                    |invocation| invocation.production,
                )
                .ok_or_else(|| anyhow!("invocation missing production"))?;

            let production = store.read(&production_id)?;

            if path.ends_with('/') {
                for (out, id) in production.outputs.iter() {
                    if out.starts_with(path) {
                        println!("{} {}", id, out);
                    }
                }
            } else {
                let mut reader = production
                    .outputs
                    .get(path)
                    .ok_or_else(|| anyhow!("output not found"))
                    .and_then(|id| store.read_resource(id))?;
                io::copy(&mut reader, &mut io::stdout())?;
            }
        }
        Command::Print { objtype, id } => match objtype {
            ObjectType::Invocation => {
                let obj = store.read(&Id::<Invocation>::new(id))?;
                attributes::to_writer(&mut io::stdout(), &obj)?;
            }
            ObjectType::Production => {
                let obj = store.read(&Id::<Production>::new(id))?;
                attributes::to_writer(&mut io::stdout(), &obj)?;
            }
            ObjectType::Job => {
                let obj = store.read(&Id::<Job>::new(id))?;
                attributes::to_writer(&mut io::stdout(), &obj)?;
            }
        },
    }

    Ok(())
}
