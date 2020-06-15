#include <ciphey/freq.hpp>

#include <boost/math/special_functions/gamma.hpp>

#include <optional>
#include <random>
#include <set>

namespace ciphey {
  float_t run_chisq(assoc_table const& assoc, freq_t count) {
    prob_t chisq = 0;

    for (auto const& elem : assoc) {
      if (elem.expected == 0)
        return std::numeric_limits<float_t>::infinity();

      prob_t contrib = elem.expected - elem.observed;
      contrib *= contrib;
      contrib /= elem.expected;

      chisq += contrib;
    }

    // (f_e/n - f_o/n)^2/(f_e/n) = (1/n) (f_e - f_o)/(f_e)
    //
    // We need to normalise this for our stat to be interpretable
    chisq *= count;
    return chisq;
  }

  float_t chisq_cdf(freq_t dof, float_t up_to) {
    return boost::math::gamma_p(static_cast<float>(dof) / 2, up_to / 2);
  }

  prob_t gof_chisq(assoc_table const& assoc, freq_t count) {
    auto stat = run_chisq(assoc, count);
    // Handle the asymptopic value
    if (stat == std::numeric_limits<float_t>::infinity())
      return 0;
    // We want the upper tail
    auto p_value = 1 - chisq_cdf(assoc.size() - 1, stat);
    return p_value;
  }

  assoc_table create_assoc_table(prob_table const& observed, prob_table const& expected) {
    // TODO: optimise
    //
    // First, let's get all the keys
    struct key_elem {
      std::optional<float_t> obs_val, exp_val;
    };

    std::map<char_t, key_elem> keys;
    for (auto& i : expected)
      keys[i.first].exp_val = i.second;
    // We can use index notation for this, as that gets default constructed
    for (auto& i : observed)
      keys[i.first].obs_val = i.second;

    // We can now fill in all the values, with non-existent expected values being defined as zero
    assoc_table ret;
    ret.reserve(keys.size());

    for (auto& i : keys)
      ret.emplace_back(assoc_table_elem{
        .observed = i.second.obs_val.value_or(0),
        .expected = i.second.exp_val.value_or(0)});

    return ret;
  }

  void freq_analysis(freq_table& tab, string_t const& str) {
    for (auto& i : str)
      ++tab[i];
  }
  prob_table freq_conv(freq_table const& freqs, freq_t total_len) {
    prob_table ret;
    for (auto& i : freqs)
      ret[i.first] = static_cast<prob_t>(i.second) / total_len;
    return ret;
  }
  prob_table freq_conv(freq_table const& freqs) {
    freq_t total_len = 0;
    for (auto& i : freqs)
      total_len += i.second;
    return freq_conv(freqs, total_len);
  }
  windowed_prob_table freq_conv(windowed_freq_table& freqs, freq_t total_len) {
    windowed_prob_table ret;
    ret.reserve(total_len);
    for (auto& i : freqs)
      // TODO: work the length out here, rather than do slow counting
      ret.emplace_back(freq_conv(i));
    return ret;
  }
  windowed_prob_table freq_conv(windowed_freq_table& freqs) {
    freq_t total_len = 0;
    for (auto& i : freqs)
      for (auto& entry: i)
        total_len += entry.second;
    return freq_conv(freqs, total_len);
  }

  size_t filter_missing(freq_table& target, prob_table const& lookup) {
    std::vector<char_t> to_remove;
    size_t ret = 0;
    for (auto& i : target) {
      if (auto iter = lookup.find(i.first); iter == lookup.end() || iter->second == 0) {
        to_remove.push_back(i.first);
        ret += i.second;
      }
    }

    for (auto i : to_remove)
      target.erase(i);

    return ret;
  }

  size_t filter_missing(freq_table& target, domain_t const& tab) {
    std::vector<char_t> to_remove;
    size_t ret = 0;
    for (auto& i : target) {
      if (!tab.count(i.first)) {
        to_remove.push_back(i.first);
        ret += i.second;
      }
    }

    for (auto i : to_remove)
      target.erase(i);

    return ret;
  }

//  size_t filter_missing(windowed_freq_table& target, prob_table const& lookup) {
//    size_t acc = 0;
//    for (auto& i : target)
//      acc += filter_missing(i, lookup);
//    return acc;
//  }

  void freq_analysis(windowed_freq_table& tabs, string_t const& str, size_t offset) {
    for (size_t i = 0; i < str.size(); ++i)
      ++tabs[(offset + i) % tabs.size()][str[i]];
  }

  size_t freq_analysis(windowed_freq_table& tabs, string_t const& str, domain_t const& domain) {
    size_t i = 0;
    for (auto& c : str) {
      if (domain.count(c)) {
        ++tabs[i % tabs.size()][c];
        ++i;
      }
    }
    return i;
  }

  string_t generate_fuzz(prob_table const& tab, size_t len) {
    string_t ret;
    ret.resize(len);
    std::mt19937 rng;

    {
      thread_local std::random_device seed_rng;
      thread_local std::uniform_int_distribution<decltype(rng)::result_type> seed_dist;
      rng.seed(seed_dist(seed_rng));
    }

    thread_local std::uniform_real_distribution<float_t> dist{0, 1};

    for (auto& rand_char : ret) {
    restat_char:
      float_t stat = dist(rng);
      // Iterate through the table, removing the probabilities until we are within a bracket
      for (auto const& i : tab) {
        if ((stat -= i.second) <= 0) {
          rand_char = i.first;
          goto next_char;
        }
      }
      // This should not happen in normal usage!
      //
      // However, floats do weird things with rounding, so we will be leniant
      goto restat_char;
      next_char: {}
    }

    return ret;
  }

  prob_t closeness_chisq(prob_table const& observed, prob_table const& expected, freq_t count) {
    assoc_table assoc;
    assoc.reserve(expected.size());
    for (auto& i : expected)
      assoc.emplace_back(assoc_table_elem{.expected = i.second});
    std::sort(assoc.rbegin(), assoc.rend(),
              [](assoc_table_elem& a, assoc_table_elem& b) { return a.expected < b.expected; });

    std::vector<prob_t> observed_sorted;
    observed_sorted.reserve(observed.size());
    for (auto& i : observed)
      observed_sorted.emplace_back(i.second);
    std::sort(observed_sorted.rbegin(), observed_sorted.rend());
    // Trim unobserved values
    while (observed_sorted.back() == 0) observed_sorted.pop_back();

    // Quick bypass to avoid more filling than we have to
    //
    // We must wait, as `observed` could contain zero values
    if (observed.size() > expected.size())
      return 0;

    // Fill table with observed values, or zeroes where appropriate
    size_t i;
    for (i = 0; i < observed.size(); ++i)
      assoc[i].observed = observed_sorted[i];
    for (; i < expected.size(); ++i)
      assoc[i].observed = 0;

    auto ret = gof_chisq(assoc, count);
    return ret;
  }
}

