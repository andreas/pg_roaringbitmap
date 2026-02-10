--
--  Test roaringbitmap extension
--

CREATE EXTENSION if not exists roaringbitmap;

-- Test input and output

set roaringbitmap.output_format='array';
set extra_float_digits = 0;

select  '{}'::roaringbitmap;
select  '  { 	 }  '::roaringbitmap;
select  '{	1 }'::roaringbitmap;
select  '{-1,2,555555,-4}'::roaringbitmap;
select  '{ -1 ,  2  , 555555 ,  -4  }'::roaringbitmap;
select  '{ 1 ,  -2  , 555555 ,  -4  }'::roaringbitmap;
select  '{ 1 ,  -2  , 555555 ,  -4  ,2147483647,-2147483648}'::roaringbitmap;
select  roaringbitmap('{ 1 ,  -2  , 555555 ,  -4  }');

set roaringbitmap.output_format='bytea';
select  '{}'::roaringbitmap;
select  '{ -1 ,  2  , 555555 ,  -4  }'::roaringbitmap;

set roaringbitmap.output_format='array';
select  '{}'::roaringbitmap;
select '\x3a30000000000000'::roaringbitmap;
select '\x3a300000030000000000000008000000ffff01002000000022000000240000000200237afcffffff'::roaringbitmap;

-- Test native deserialization (CROARING native format via roaring_bitmap_deserialize_safe)
select rb_deserialize_native('\x0100000000'::bytea);
select rb_deserialize_native('\x010100000001000000'::bytea);
select rb_deserialize_native('\x010200000001000000e8030000'::bytea);
select rb_deserialize_native('\x023a30000000000000'::bytea);

-- Exception
select  ''::roaringbitmap;
select  '{'::roaringbitmap;
select  '{1'::roaringbitmap;
select  '{1} x'::roaringbitmap;
select  '{1x}'::roaringbitmap;
select  '{-x}'::roaringbitmap;
select  '{,}'::roaringbitmap;
select  '{1,}'::roaringbitmap;
select  '{1,xxx}'::roaringbitmap;
select  '{1,3'::roaringbitmap;
select  '{1,1'::roaringbitmap;
select  '{1,-2147483649}'::roaringbitmap;
select  '{2147483648}'::roaringbitmap;

-- Test Type cast
select '{}'::roaringbitmap::bytea;
select '{1}'::roaringbitmap::bytea;
select '{1,9999}'::roaringbitmap::bytea;
select '{}'::roaringbitmap::bytea::roaringbitmap;
select '{1}'::roaringbitmap::bytea::roaringbitmap;
select '{1,9999,-88888}'::roaringbitmap::bytea::roaringbitmap;
select roaringbitmap('{1,9999,-88888}'::roaringbitmap::bytea);

-- Exception
select roaringbitmap('\x11'::bytea);
select '\x11'::bytea::roaringbitmap;

-- Test Opperator

select roaringbitmap('{}') & roaringbitmap('{}');
select roaringbitmap('{}') & roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') & roaringbitmap('{}');
select roaringbitmap('{1,2,3}') & roaringbitmap('{3,4,5}');
select roaringbitmap('{1,-2,-3}') & roaringbitmap('{-3,-4,5}');

select roaringbitmap('{}') | roaringbitmap('{}');
select roaringbitmap('{}') | roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') | roaringbitmap('{}');
select roaringbitmap('{1,2,3}') | roaringbitmap('{3,4,5}');
select roaringbitmap('{1,-2,-3}') | roaringbitmap('{-3,-4,5}');

select roaringbitmap('{}') | 6;
select roaringbitmap('{1,2,3}') | 6;
select roaringbitmap('{1,2,3}') | 1;
select roaringbitmap('{1,2,3}') | -1;
select roaringbitmap('{-1,-2,3}') | -1;

select 6 | roaringbitmap('{}');
select 6 | roaringbitmap('{1,2,3}');
select 1 | roaringbitmap('{1,2,3}');
select -1 | roaringbitmap('{1,2,3}');
select -1 | roaringbitmap('{-1,-2,3}');

select roaringbitmap('{}') # roaringbitmap('{}');
select roaringbitmap('{}') # roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') # roaringbitmap('{}');
select roaringbitmap('{1,2,3}') # roaringbitmap('{3,4,5}');
select roaringbitmap('{1,-2,-3}') # roaringbitmap('{-3,-4,5}');

select roaringbitmap('{}') - roaringbitmap('{}');
select roaringbitmap('{}') - roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') - roaringbitmap('{}');
select roaringbitmap('{1,2,3}') - roaringbitmap('{3,4,5}');
select roaringbitmap('{1,-2,-3}') - roaringbitmap('{-3,-4,5}');

select roaringbitmap('{}') - 3;
select roaringbitmap('{1,2,3}') - 3;
select roaringbitmap('{1,2,3}') - 1;
select roaringbitmap('{1,2,3}') - -1;
select roaringbitmap('{-1,-2,3}') - -1;

select roaringbitmap('{}') << 2;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << 2;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << 1;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << 0;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << -1;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << -2;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << 4294967295;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << 4294967296;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << -4294967295;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') << -4294967296;

select roaringbitmap('{}') >> 2;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> 2;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> 1;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> 0;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> -1;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> -2;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> 4294967295;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> 4294967296;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> -4294967295;
select roaringbitmap('{-2,-1,0,1,2,3,2147483647,-2147483648}') >> -4294967296;

select roaringbitmap('{}') @> roaringbitmap('{}');
select roaringbitmap('{}') @> roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') @> roaringbitmap('{}');
select roaringbitmap('{1,2,3}') @> roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') @> roaringbitmap('{3,2}');
select roaringbitmap('{1,-2,-3}')  @> roaringbitmap('{-3,1}');

select roaringbitmap('{}') @> 2;
select roaringbitmap('{1,2,3}') @> 20;
select roaringbitmap('{1,2,3}') @> 1;
select roaringbitmap('{1,2,3}') @> -1;
select roaringbitmap('{-1,-2,3}') @> -1;

select roaringbitmap('{}') <@ roaringbitmap('{}');
select roaringbitmap('{}') <@ roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') <@ roaringbitmap('{}');
select roaringbitmap('{1,2,3}') <@ roaringbitmap('{3,4,5}');
select roaringbitmap('{2,3}') <@ roaringbitmap('{1,3,2}');
select roaringbitmap('{1,-3}')  <@ roaringbitmap('{-3,1,1000}');

select 6 <@ roaringbitmap('{}');
select 3 <@ roaringbitmap('{1,2,3}');
select 1 <@ roaringbitmap('{1,2,3}');
select -1 <@ roaringbitmap('{1,2,3}');
select -1 <@ roaringbitmap('{-1,-2,3}');

select roaringbitmap('{}') && roaringbitmap('{}');
select roaringbitmap('{}') && roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') && roaringbitmap('{}');
select roaringbitmap('{1,2,3}') && roaringbitmap('{3,4,5}');
select roaringbitmap('{1,-2,-3}') && roaringbitmap('{-3,-4,5}');

select roaringbitmap('{}') = roaringbitmap('{}');
select roaringbitmap('{}') = roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') = roaringbitmap('{}');
select roaringbitmap('{1,2,3}') = roaringbitmap('{3,1,2}');
select roaringbitmap('{1,-2,-3}') = roaringbitmap('{-3,-4,5}');

select roaringbitmap('{}') <> roaringbitmap('{}');
select roaringbitmap('{}') <> roaringbitmap('{3,4,5}');
select roaringbitmap('{1,2,3}') <> roaringbitmap('{}');
select roaringbitmap('{1,2,3}') <> roaringbitmap('{3,1,2}');
select roaringbitmap('{1,-2,-3}') <> roaringbitmap('{-3,-4,5}');

-- Test the functions with one bitmap variable

select rb_build(NULL);
select rb_build('{}'::int[]);
select rb_build('{1}'::int[]);
select rb_build('{-1,2,555555,-4}'::int[]);
select rb_build('{1,-2,555555,-4,2147483647,-2147483648}'::int[]);

select rb_to_array(NULL);
select rb_to_array('{}'::roaringbitmap);
select rb_to_array('{1}'::roaringbitmap);
select rb_to_array('{-1,2,555555,-4}'::roaringbitmap);
select rb_to_array('{1,-2,555555,-4,2147483647,-2147483648}'::roaringbitmap);

select rb_is_empty(NULL);
select rb_is_empty('{}');
select rb_is_empty('{1}');
select rb_is_empty('{1,10,100}');

select rb_cardinality(NULL);
select rb_cardinality('{}');
select rb_cardinality('{1}');
select rb_cardinality('{1,10,100}');

select rb_max(NULL);
select rb_max('{}');
select rb_max('{1}');
select rb_max('{1,10,100}');
select rb_max('{1,10,100,2147483647,-2147483648,-1}');

select rb_min(NULL);
select rb_min('{}');
select rb_min('{1}');
select rb_min('{1,10,100}');
select rb_min('{1,10,100,2147483647,-2147483648,-1}');

select rb_iterate(NULL);
select rb_iterate('{}');
select rb_iterate('{1}');
select rb_iterate('{1,10,100}');
select rb_iterate('{1,10,100,2147483647,-2147483648,-1}');

-- Test the functions with two bitmap variables

select rb_and(NULL,'{1,10,100}');
select rb_and('{1,10,100}',NULL);
select rb_and('{}','{1,10,100}');
select rb_and('{1,10,100}','{}');
select rb_and('{2}','{1,10,100}');
select rb_and('{1,2,10}','{1,10,100}');
select rb_and('{1,10}','{1,10,100}');

select rb_and_cardinality(NULL,'{1,10,100}');
select rb_and_cardinality('{1,10,100}',NULL);
select rb_and_cardinality('{}','{1,10,100}');
select rb_and_cardinality('{1,10,100}','{}');
select rb_and_cardinality('{2}','{1,10,100}');
select rb_and_cardinality('{1,2,10}','{1,10,100}');
select rb_and_cardinality('{1,10}','{1,10,100}');

select rb_or(NULL,'{1,10,100}');
select rb_or('{1,10,100}',NULL);
select rb_or('{}','{1,10,100}');
select rb_or('{1,10,100}','{}');
select rb_or('{2}','{1,10,100}');
select rb_or('{1,2,10}','{1,10,100}');
select rb_or('{1,10}','{1,10,100}');

select rb_or_cardinality(NULL,'{1,10,100}');
select rb_or_cardinality('{1,10,100}',NULL);
select rb_or_cardinality('{}','{1,10,100}');
select rb_or_cardinality('{1,10,100}','{}');
select rb_or_cardinality('{2}','{1,10,100}');
select rb_or_cardinality('{1,2,10}','{1,10,100}');
select rb_or_cardinality('{1,10}','{1,10,100}');

select rb_xor(NULL,'{1,10,100}');
select rb_xor('{1,10,100}',NULL);
select rb_xor('{}','{1,10,100}');
select rb_xor('{1,10,100}','{}');
select rb_xor('{2}','{1,10,100}');
select rb_xor('{1,2,10}','{1,10,100}');
select rb_xor('{1,10}','{1,10,100}');

select rb_xor_cardinality(NULL,'{1,10,100}');
select rb_xor_cardinality('{1,10,100}',NULL);
select rb_xor_cardinality('{}','{1,10,100}');
select rb_xor_cardinality('{1,10,100}','{}');
select rb_xor_cardinality('{2}','{1,10,100}');
select rb_xor_cardinality('{1,2,10}','{1,10,100}');
select rb_xor_cardinality('{1,10}','{1,10,100}');

select rb_equals(NULL,'{1,10,100}');
select rb_equals('{1,10,100}',NULL);
select rb_equals('{}','{1,10,100}');
select rb_equals('{1,10,100}','{}');
select rb_equals('{2}','{1,10,100}');
select rb_equals('{1,2,10}','{1,10,100}');
select rb_equals('{1,10}','{1,10,100}');
select rb_equals('{1,10,100}','{1,10,100}');
select rb_equals('{1,10,100,10}','{1,100,10}');

select rb_intersect(NULL,'{1,10,100}');
select rb_intersect('{1,10,100}',NULL);
select rb_intersect('{}','{1,10,100}');
select rb_intersect('{1,10,100}','{}');
select rb_intersect('{2}','{1,10,100}');
select rb_intersect('{1,2,10}','{1,10,100}');
select rb_intersect('{1,10}','{1,10,100}');
select rb_intersect('{1,10,100}','{1,10,100}');
select rb_intersect('{1,10,100,10}','{1,100,10}');

select rb_andnot(NULL,'{1,10,100}');
select rb_andnot('{1,10,100}',NULL);
select rb_andnot('{}','{1,10,100}');
select rb_andnot('{1,10,100}','{}');
select rb_andnot('{2}','{1,10,100}');
select rb_andnot('{1,2,10}','{1,10,100}');
select rb_andnot('{1,10}','{1,10,100}');
select rb_andnot('{1,10,100}','{1,10,100}');
select rb_andnot('{1,10,100,10}','{1,100,10}');

select rb_andnot_cardinality(NULL,'{1,10,100}');
select rb_andnot_cardinality('{1,10,100}',NULL);
select rb_andnot_cardinality('{}','{1,10,100}');
select rb_andnot_cardinality('{1,10,100}','{}');
select rb_andnot_cardinality('{2}','{1,10,100}');
select rb_andnot_cardinality('{1,2,10}','{1,10,100}');
select rb_andnot_cardinality('{1,10}','{1,10,100}');
select rb_andnot_cardinality('{1,10,100}','{1,10,100}');
select rb_andnot_cardinality('{1,10,100,10}','{1,100,10}');

select rb_jaccard_dist(NULL,'{1,10,100}');
select rb_jaccard_dist('{1,10,100}',NULL);
select rb_jaccard_dist('{}','{1,10,100}');
select rb_jaccard_dist('{1,10,100}','{}');
select rb_jaccard_dist('{2}','{1,10,100}');
select rb_jaccard_dist('{1,2,10}','{1,10,100}');
select rb_jaccard_dist('{1,10,11,12}','{1,10,100}');
select rb_jaccard_dist('{1,10,100}','{1,10,11,12}');
select rb_jaccard_dist('{1,10,100}','{1,10,100}');
select rb_jaccard_dist('{1,10,-100}','{1,10,-100}');
select rb_jaccard_dist('{1,10,100}','{1,10,-100}');

-- Test other functions

select rb_rank(NULL,0);
select rb_rank('{}',0);
select rb_rank('{1,10,100}',0);
select rb_rank('{1,10,100}',1);
select rb_rank('{1,10,100}',99);
select rb_rank('{1,10,100}',100);
select rb_rank('{1,10,100}',101);
select rb_rank('{1,10,100,-3,-1}',-2);

select rb_remove(NULL,0);
select rb_remove('{}',0);
select rb_remove('{1}',1);
select rb_remove('{1,10,100}',0);
select rb_remove('{1,10,100}',1);
select rb_remove('{1,10,100}',99);

select rb_fill(NULL,0,0);
select rb_fill('{}',0,0);
select rb_fill('{}',0,1);
select rb_fill('{}',0,2);
select rb_fill('{1,10,100}',10,10);
select rb_fill('{1,10,100}',10,11);
select rb_fill('{1,10,100}',10,12);
select rb_fill('{1,10,100}',10,13);
select rb_fill('{1,10,100}',10,20);
select rb_fill('{1,10,100}',0,-1);
select rb_cardinality(rb_fill('{1,10,100}',2,1000000000));
select rb_cardinality(rb_fill('{1,10,100}',-1,5000000000));

select rb_index(NULL,3);
select rb_index('{1,2,3}',NULL);
select rb_index('{}',3);
select rb_index('{1}',3);
select rb_index('{1}',1);
select rb_index('{1,10,100}',10);
select rb_index('{1,10,100}',99);
select rb_index('{1,10,-100}',-100);

select rb_clear(NULL,0,10);
select rb_clear('{}',0,10);
select rb_clear('{1,10,100}',0,10);
select rb_clear('{1,10,100}',3,3);
select rb_clear('{1,10,100}',-3,3);
select rb_clear('{1,10,100}',0,-1);
select rb_clear('{1,10,100}',9,9);
select rb_clear('{1,10,100}',2,1000000000);
select rb_clear('{0,1,10,100,-2,-1}',1,4294967295);
select rb_clear('{0,1,10,100,-2,-1}',0,4294967296);

select rb_flip(NULL,0,10);
select rb_flip('{}',0,10);
select rb_flip('{1,10,100}',9,100);
select rb_flip('{1,10,100}',10,101);
select rb_flip('{1,10,100}',-3,3);
select rb_flip('{1,10,100}',0,-1);
select rb_flip('{1,10,100}',9,9);
select rb_cardinality(rb_flip('{1,10,100}',2,1000000000));
select rb_cardinality(rb_flip('{1,10,100}',-1,5000000000));

select rb_range(NULL,0,10);
select rb_range('{}',0,10);
select rb_range('{1,10,100}',0,10);
select rb_range('{1,10,100}',3,3);
select rb_range('{1,10,100}',-3,3);
select rb_range('{1,10,100}',0,-1);
select rb_range('{1,10,100}',9,9);
select rb_range('{1,10,100}',2,1000000000);
select rb_range('{0,1,10,100,-2,-1}',1,4294967295);
select rb_range('{0,1,10,100,-2,-1}',0,4294967296);

select rb_range_cardinality(NULL,0,10);
select rb_range_cardinality('{}',0,10);
select rb_range_cardinality('{1,10,100}',0,10);
select rb_range_cardinality('{1,10,100}',3,3);
select rb_range_cardinality('{1,10,100}',-3,3);
select rb_range_cardinality('{1,10,100}',0,-1);
select rb_range_cardinality('{1,10,100}',9,9);
select rb_range_cardinality('{1,10,100}',2,1000000000);
select rb_range_cardinality('{0,1,10,100,-2,-1}',1,4294967295);
select rb_range_cardinality('{0,1,10,100,-2,-1}',0,4294967296);

select rb_select(NULL,10);
select rb_select('{}',10);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',0);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',1);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,0);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,true);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,true,9);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,true,9,4294967295);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false,9);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false,10);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false,10,10);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false,-10,100);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false,-10,-10);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,false,10,10001);
select rb_select('{0,1,2,10,100,1000,2147483647,-2147483648,-2,-1}',2,1,true,10,10001);


-- Test aggregate

select rb_and_agg(id) from (values (NULL::roaringbitmap)) t(id);
select rb_and_agg(id) from (values (roaringbitmap('{}'))) t(id);
select rb_and_agg(id) from (values (roaringbitmap('{1}'))) t(id);
select rb_and_agg(id) from (values (roaringbitmap('{1,10,100}'))) t(id);
select rb_and_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_and_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{1,10,100}'))) t(id);
select rb_and_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{}'))) t(id);
select rb_and_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{1}'))) t(id);
select rb_and_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2}'))) t(id);
select rb_and_agg(id) from (values (roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{2,10}'))) t(id);
select rb_and_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,100,101}')),(NULL)) t(id);

select rb_and_cardinality_agg(id) from (values (NULL::roaringbitmap)) t(id);
select rb_and_cardinality_agg(id) from (values (roaringbitmap('{}'))) t(id);
select rb_and_cardinality_agg(id) from (values (roaringbitmap('{1}'))) t(id);
select rb_and_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}'))) t(id);
select rb_and_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_and_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{1,10,100}'))) t(id);
select rb_and_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{}'))) t(id);
select rb_and_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{1}'))) t(id);
select rb_and_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2}'))) t(id);
select rb_and_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{2,10}'))) t(id);
select rb_and_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,100,101}')),(NULL)) t(id);

select rb_or_agg(id) from (values (NULL::roaringbitmap)) t(id);
select rb_or_agg(id) from (values (roaringbitmap('{}'))) t(id);
select rb_or_agg(id) from (values (roaringbitmap('{1}'))) t(id);
select rb_or_agg(id) from (values (roaringbitmap('{1,10,100}'))) t(id);
select rb_or_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_or_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{1,10,100}'))) t(id);
select rb_or_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{}'))) t(id);
select rb_or_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{1}'))) t(id);
select rb_or_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2}'))) t(id);
select rb_or_agg(id) from (values (roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{2,10}'))) t(id);
select rb_or_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,100,101}')),(NULL)) t(id);

select rb_or_cardinality_agg(id) from (values (NULL::roaringbitmap)) t(id);
select rb_or_cardinality_agg(id) from (values (roaringbitmap('{}'))) t(id);
select rb_or_cardinality_agg(id) from (values (roaringbitmap('{1}'))) t(id);
select rb_or_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}'))) t(id);
select rb_or_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_or_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{1,10,100}'))) t(id);
select rb_or_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{}'))) t(id);
select rb_or_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{1}'))) t(id);
select rb_or_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2}'))) t(id);
select rb_or_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{2,10}'))) t(id);
select rb_or_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,100,101}')),(NULL)) t(id);

select rb_xor_agg(id) from (values (NULL::roaringbitmap)) t(id);
select rb_xor_agg(id) from (values (roaringbitmap('{}'))) t(id);
select rb_xor_agg(id) from (values (roaringbitmap('{1}'))) t(id);
select rb_xor_agg(id) from (values (roaringbitmap('{1,10,100}'))) t(id);
select rb_xor_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_xor_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{1,10,100}'))) t(id);
select rb_xor_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{}'))) t(id);
select rb_xor_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{1}'))) t(id);
select rb_xor_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2}'))) t(id);
select rb_xor_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_xor_agg(id) from (values (roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,100,101}'))) t(id);
select rb_xor_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,101}')),(roaringbitmap('{1,100,102}')),(NULL)) t(id);

select rb_xor_cardinality_agg(id) from (values (NULL::roaringbitmap)) t(id);
select rb_xor_cardinality_agg(id) from (values (roaringbitmap('{}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (roaringbitmap('{1}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(roaringbitmap('{1,10,100}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{1}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(roaringbitmap('{2,10}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,100,101}'))) t(id);
select rb_xor_cardinality_agg(id) from (values (NULL),(roaringbitmap('{1,10,100}')),(NULL),(roaringbitmap('{1,10,101}')),(roaringbitmap('{1,100,102}')),(NULL)) t(id);

select rb_build_agg(id) from (values (NULL::int)) t(id);
select rb_build_agg(id) from (values (1)) t(id);
select rb_build_agg(id) from (values (1),(10)) t(id);
select rb_build_agg(id) from (values (1),(10),(10),(100),(1)) t(id);


-- Test Windows aggregate

with t(id,bitmap) as(
 values(0,NULL),(1,roaringbitmap('{1,10}')),(2,NULL),(3,roaringbitmap('{2,10}')),(4,roaringbitmap('{10,100}'))
)
select id,bitmap,rb_and_agg(bitmap) over(order by id),rb_and_cardinality_agg(bitmap) over(order by id) from t;

with t(id,bitmap) as(
 values(0,NULL),(1,roaringbitmap('{1,10}')),(2,NULL),(3,roaringbitmap('{2,10}')),(4,roaringbitmap('{10,100}'))
)
select id,bitmap,rb_or_agg(bitmap) over(order by id),rb_or_cardinality_agg(bitmap) over(order by id) from t;

with t(id,bitmap) as(
 values(0,NULL),(1,roaringbitmap('{1,10}')),(2,NULL),(3,roaringbitmap('{2,10}')),(4,roaringbitmap('{10,100}'))
)
select id,bitmap,rb_xor_agg(bitmap) over(order by id),rb_xor_cardinality_agg(bitmap) over(order by id) from t;

with t(id) as(
 values(0),(1),(2),(NULL),(4),(NULL)
)
select id,rb_build_agg(id) over(order by id) from t;


-- Test parallel aggregate

set max_parallel_workers=8;
set max_parallel_workers_per_gather=2;
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;

CREATE OR REPLACE FUNCTION get_json_plan(sql text) RETURNS SETOF json AS
$BODY$
BEGIN
    RETURN QUERY EXECUTE 'EXPLAIN (COSTS OFF,FORMAT JSON) ' || sql;

    RETURN;
 END
$BODY$
LANGUAGE plpgsql;


drop table if exists bitmap_test_tb1;
create table bitmap_test_tb1(id int, bitmap roaringbitmap);
insert into bitmap_test_tb1 values (NULL,NULL);
insert into bitmap_test_tb1 select id,rb_build(ARRAY[id]) from generate_series(1,10000)id;
insert into bitmap_test_tb1 values (NULL,NULL);
insert into bitmap_test_tb1 values (10001,rb_build(ARRAY[10,100,1000,10000,10001]));

select position('"Parallel Aware": true' in get_json_plan('
select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_build_agg(id) bitmap from bitmap_test_tb1)a
')::text) > 0 is_parallel_plan;

select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_build_agg(id) bitmap from bitmap_test_tb1)a;

select position('"Parallel Aware": true' in get_json_plan('
select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_and_agg(bitmap) bitmap from bitmap_test_tb1)a
')::text) > 0 is_parallel_plan;

select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_and_agg(bitmap) bitmap from bitmap_test_tb1)a;

select position('"Parallel Aware": true' in get_json_plan('
select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_or_agg(bitmap) bitmap from bitmap_test_tb1)a
')::text) > 0 is_parallel_plan;

select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_or_agg(bitmap) bitmap from bitmap_test_tb1)a;

select position('"Parallel Aware": true' in get_json_plan('
select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_xor_agg(bitmap) bitmap from bitmap_test_tb1)a
')::text) > 0 is_parallel_plan;

select rb_cardinality(bitmap),rb_min(bitmap),rb_max(bitmap)
  from (select rb_xor_agg(bitmap) bitmap from bitmap_test_tb1)a;

select position('"Parallel Aware": true' in get_json_plan('
select rb_and_cardinality_agg(bitmap),rb_or_cardinality_agg(bitmap),rb_xor_cardinality_agg(bitmap) from bitmap_test_tb1
')::text) > 0 is_parallel_plan;

select rb_and_cardinality_agg(bitmap),rb_or_cardinality_agg(bitmap),rb_xor_cardinality_agg(bitmap) from bitmap_test_tb1;

--rb_iterate() not support parallel on PG10 while run on parallel in PG11+
--explain(costs off) 
--select count(*) from (select rb_iterate(bitmap) from bitmap_test_tb1)a;

select count(*) from (select rb_iterate(bitmap) from bitmap_test_tb1)a;

select position('"Parallel Aware": true' in get_json_plan('
select id,bitmap,
  rb_build_agg(id) over(w),
  rb_or_agg(bitmap) over(w),
  rb_and_agg(bitmap) over(w),
  rb_xor_agg(bitmap) over(w) 
from bitmap_test_tb1
window w as (order by id)
order by id limit 10
')::text) > 0 is_parallel_plan;

select id,bitmap,
  rb_build_agg(id) over(w),
  rb_or_agg(bitmap) over(w),
  rb_and_agg(bitmap) over(w),
  rb_xor_agg(bitmap) over(w) 
from bitmap_test_tb1
window w as (order by id)
order by id limit 10;

-- bugfix #22
SELECT '{100373,1829130,1861002,1975442,2353213,2456403}'::roaringbitmap & '{2353213}'::roaringbitmap;
SELECT rb_and_cardinality('{100373,1829130,1861002,1975442,2353213,2456403}','{2353213}');

-- bugfix #45: rb_to_array and rb_min exhibits inconsistent results in a malformed serialize input
select rb_to_array('\x3a300000010000000000000000000000000000000000000000000000000000000000000000000000000000000008'::roaringbitmap),
rb_min('\x3a300000010000000000000000000000000000000000000000000000000000000000000000000000000000000008'::roaringbitmap);

select rb_kmerge(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
]);


select rb_kmerge_avg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[1,2,3]);


select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[1,2,3], 'avg(double precision)'::regprocedure) as t(result float8);

select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[1.5,2.5,3.5], 'avg(double precision)'::regprocedure) as t(result float8);

-- Test every(boolean): boolean input/output
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[true,false,true], 'every(boolean)'::regprocedure) as t(result boolean);

-- Test sum(integer): integer input, bigint output
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[10,20,30], 'sum(integer)'::regprocedure) as t(result bigint);

-- Test sum(double precision): double input/output
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[1.1,2.2,3.3], 'sum(double precision)'::regprocedure) as t(result double precision);

-- Test max(integer): integer input/output
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[100,200,300], 'max(integer)'::regprocedure) as t(result integer);

-- Test array_agg(anynonarray): integer input, integer[] output (uses internal transtype)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[10,20,30], 'array_agg(anynonarray)'::regprocedure) as t(result integer[]);

-- Test array_agg(anynonarray): text input, text[] output (uses internal transtype)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY['apple','banana','cherry'], 'array_agg(anynonarray)'::regprocedure) as t(result text[]);

-- ============================================================
-- Edge case tests for rb_kmerge_agg
-- ============================================================

-- Edge case: Empty bitmap array - should return no rows
select * from rb_kmerge_agg(ARRAY[]::roaringbitmap[], ARRAY[]::integer[], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: All NULL bitmaps - should return no rows (NULLs are skipped)
select * from rb_kmerge_agg(ARRAY[NULL,NULL,NULL]::roaringbitmap[], ARRAY[1,2,3], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: Some NULL bitmaps mixed with non-NULL - NULLs skipped
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3]),
  NULL,
  rb_build(ARRAY[3,5])
], ARRAY[10,20,30], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: Empty bitmaps (no bits set) - should return no rows for those
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[]::integer[]),
  rb_build(ARRAY[1,2]),
  rb_build(ARRAY[]::integer[])
], ARRAY[10,20,30], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: Single bitmap
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2,3])
], ARRAY[100], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: Single element bitmap
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[42])
], ARRAY[999], 'sum(integer)'::regprocedure) as t(result bigint);

-- Error case: count doesn't have a typed variant like count(integer)
-- (PostgreSQL's count is only count(*) or count(any) which doesn't resolve to a specific OID)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[10,20,30], 'count(integer)'::regprocedure) as t(result bigint);

-- Edge case: min aggregate
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY[100,200,300], 'min(integer)'::regprocedure) as t(result integer);

-- Edge case: Type coercion - integer labels with avg (needs double precision)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3]),
  rb_build(ARRAY[3,5])
], ARRAY[10,20], 'avg(double precision)'::regprocedure) as t(result float8);

-- Edge case: bigint labels with sum(bigint)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2]),
  rb_build(ARRAY[2,3])
], ARRAY[1000000000000::bigint, 2000000000000::bigint], 'sum(bigint)'::regprocedure) as t(result numeric);

-- Error case: string_agg takes 2 arguments (value, separator) which we don't support
-- rb_kmerge_agg only supports single-argument aggregates
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
], ARRAY['a','b','c'], 'string_agg(text,text)'::regprocedure) as t(result text);

-- Error case: Mismatched array lengths (more labels than bitmaps)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2])
], ARRAY[10,20,30], 'sum(integer)'::regprocedure) as t(result bigint);

-- Error case: Mismatched array lengths (more bitmaps than labels)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[5,6])
], ARRAY[10], 'sum(integer)'::regprocedure) as t(result bigint);

-- Error case: NULL label with non-NULL bitmap
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2]),
  rb_build(ARRAY[3,4])
], ARRAY[10,NULL], 'sum(integer)'::regprocedure) as t(result bigint);

-- Error case: Invalid aggregate OID
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2])
], ARRAY[10], 0::regprocedure) as t(result bigint);

-- Error case: Function that is not an aggregate
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2])
], ARRAY[10], 'abs(integer)'::regprocedure) as t(result integer);

-- Error case: Aggregate with wrong number of arguments (zero args)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2])
], ARRAY[10], 'count(*)'::regprocedure) as t(result bigint);

-- Error case: Wrong output column count
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2])
], ARRAY[10], 'sum(integer)'::regprocedure) as t(a bigint, b bigint);

-- Edge case: numeric type (arbitrary precision)
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2]),
  rb_build(ARRAY[2,3])
], ARRAY[1.23456789012345::numeric, 9.87654321098765::numeric], 'sum(numeric)'::regprocedure) as t(result numeric);

-- Edge case: Overlapping values in all bitmaps
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,2,3]),
  rb_build(ARRAY[1,2,3]),
  rb_build(ARRAY[1,2,3])
], ARRAY[10,20,30], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: Large overlapping set
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1]),
  rb_build(ARRAY[1]),
  rb_build(ARRAY[1]),
  rb_build(ARRAY[1]),
  rb_build(ARRAY[1])
], ARRAY[1,2,3,4,5], 'sum(integer)'::regprocedure) as t(result bigint);

-- Edge case: bit_or aggregate
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3]),
  rb_build(ARRAY[3,5])
], ARRAY[1,2], 'bit_or(integer)'::regprocedure) as t(result integer);

-- Edge case: bit_and aggregate
select * from rb_kmerge_agg(ARRAY[
  rb_build(ARRAY[1,3]),
  rb_build(ARRAY[3,5])
], ARRAY[3,7], 'bit_and(integer)'::regprocedure) as t(result integer);

-- ============================================================
-- rb_kmerge_groups tests
-- ============================================================

-- Basic: same data as rb_kmerge test
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[1,3,5]),
  rb_build(ARRAY[3,4]),
  rb_build(ARRAY[10,5])
]);

-- Grouping: many elements share the same source-set
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[1,2,3,4,5]),
  rb_build(ARRAY[1,2,3])
]);

-- Realistic: 3 question bitmaps with overlapping employee sets
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[1,2,3,4,5,6,7,8,9,10]),
  rb_build(ARRAY[1,2,3,4,5,11,12]),
  rb_build(ARRAY[1,2,3,13,14,15])
]);

-- All elements in all bitmaps -> single group
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[1,2,3]),
  rb_build(ARRAY[1,2,3]),
  rb_build(ARRAY[1,2,3])
]);

-- Empty array -> no rows
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[]::roaringbitmap[]);

-- All NULL bitmaps -> no rows
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[NULL,NULL,NULL]::roaringbitmap[]);

-- Some NULL bitmaps (skipped, remaining get consecutive indices)
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[1,3]),
  NULL,
  rb_build(ARRAY[3,5])
]);

-- Empty bitmaps (no bits set)
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[]::integer[]),
  rb_build(ARRAY[1,2]),
  rb_build(ARRAY[]::integer[])
]);

-- Single bitmap
select sources, rb_to_array(members) from rb_kmerge_groups(ARRAY[
  rb_build(ARRAY[1,2,3])
]);

-- N > 64: exercise variable-width bitmask (65 inputs, requires 2 uint64 words)
-- Build 65 bitmaps where element 1 is in all, element 2 only in bitmap 1, element 3 only in bitmap 65
select sources, rb_to_array(members) from rb_kmerge_groups(
  (select array_agg(
    case
      when i = 1 then rb_build(ARRAY[1,2])
      when i = 65 then rb_build(ARRAY[1,3])
      else rb_build(ARRAY[1])
    end order by i
  ) from generate_series(1, 65) i)
);

-- N = 100: larger than 64, element 99 only in bitmaps 1 and 100
select sources, rb_to_array(members) from rb_kmerge_groups(
  (select array_agg(
    case
      when i = 1 then rb_build(ARRAY[42, 99])
      when i = 100 then rb_build(ARRAY[42, 99])
      else rb_build(ARRAY[42])
    end order by i
  ) from generate_series(1, 100) i)
);

-- N = 128: exactly 2 words boundary, all bitmaps contain element 7
select count(*), (select count(distinct sources::text) from rb_kmerge_groups(
  (select array_agg(rb_build(ARRAY[7]) order by i) from generate_series(1, 128) i)
)) as n_groups from generate_series(1,1);

-- N = 65, disjoint: each bitmap has a unique element -> 65 groups
select count(*) from rb_kmerge_groups(
  (select array_agg(rb_build(ARRAY[i]) order by i) from generate_series(1, 65) i)
);