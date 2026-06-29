create table grade (name char(4),id int,score float);
create table grade (name char(4),id int,score float);
drop table missing_table;
insert into grade values ('Data', 1);
select bad_col from grade;
select * from grade where bad_col = 1;
