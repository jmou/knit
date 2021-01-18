select name, sum(score)
from "@STDOUT(scores.csv)"
group by name
order by sum(score) desc;
